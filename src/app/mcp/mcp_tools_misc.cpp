// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.
//
// Portions derived from diivi/aseprite-mcp (MIT); see THIRD_PARTY_NOTICES.md.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/mcp/mcp_tools_lua.h"
#include "app/mcp/mcp_tools_registry.h"

#include "base/fs.h"

#include <string>

#ifdef ENABLE_WEBSOCKET
  #include "base/file_content.h"

  #include <ixwebsocket/IXHttpServer.h>
  #include <ixwebsocket/IXNetSystem.h>

  #include <cctype>
  #include <map>
  #include <memory>
  #include <mutex>
#endif

namespace app {
namespace mcp {

namespace {

// A snippet that reports a validation failure; the "ERROR:" prefix
// turns the tool result into an error.
std::string luaError(const std::string& message)
{
  return "print(\"ERROR:" + luaString(message) + "\")";
}

#ifdef ENABLE_WEBSOCKET

ix::HttpResponsePtr previewResponse(const int status, const char* description,
                                    const std::string& body, const std::string& contentType)
{
  ix::WebSocketHttpHeaders headers;
  headers["Content-Type"] = contentType;
  return std::make_shared<ix::HttpResponse>(status, description, ix::HttpErrorCode::Ok,
                                            headers, body);
}

std::string fileContentType(const std::string& filename)
{
  std::string ext = base::get_file_extension(filename);
  for (char& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  if (ext == "html")
    return "text/html";
  if (ext == "png")
    return "image/png";
  if (ext == "gif")
    return "image/gif";
  if (ext == "json")
    return "application/json";
  return "application/octet-stream";
}

ix::HttpResponsePtr serveFile(const std::string& directory, const std::string& uri)
{
  // A query string/fragment is not part of the file name, and the site
  // root maps to index.html.
  std::string path = uri.substr(0, uri.find_first_of("?#"));
  if (path.empty() || path == "/")
    path = "/index.html";

  // Only files inside "directory" are served (no ".." escapes).
  const std::string root = base::normalize_path(directory);
  const std::string filename = base::normalize_path(base::join_path(root, path));
  std::string prefix = root;
  if (prefix.empty() || !base::is_path_separator(prefix.back()))
    prefix.push_back(base::path_separator);
  if (filename.compare(0, prefix.size(), prefix) != 0)
    return previewResponse(403, "Forbidden", "Forbidden", "text/plain");
  if (!base::is_file(filename))
    return previewResponse(404, "Not Found", "Not found", "text/plain");

  const base::buffer data = base::read_file_content(filename);
  return previewResponse(200, "OK", std::string(data.begin(), data.end()),
                         fileContentType(filename));
}

// Serves the files of a directory over HTTP on 127.0.0.1, to preview
// exported sprites. One instance owns one ix::HttpServer.
class PreviewServer {
public:
  PreviewServer(const int port, const std::string& directory)
  {
    // The scripting WebSocket class initializes the net system on the
    // first connection; do the same before creating the server.
    static std::once_flag netOnce;
    std::call_once(netOnce, &ix::initNetSystem);

    auto server = std::make_unique<ix::HttpServer>(port, "127.0.0.1");
    server->setOnConnectionCallback(
      [directory](ix::HttpRequestPtr request,
                  std::shared_ptr<ix::ConnectionState>) -> ix::HttpResponsePtr {
        return serveFile(directory, request->uri);
      });

    const std::pair<bool, std::string> listenResult = server->listen();
    if (!listenResult.first) {
      m_error = listenResult.second;
      return;
    }

    m_server = std::move(server);
    m_server->start();
  }

  ~PreviewServer() { stop(); }

  bool isRunning() const { return m_server != nullptr; }
  const std::string& error() const { return m_error; }

  void stop()
  {
    if (!m_server)
      return;
    m_server->stop();
    m_server.reset();
  }

private:
  std::unique_ptr<ix::HttpServer> m_server;
  std::string m_error;
};

// Running servers by port. MCP tool calls are serialized in every
// mode, so no lock is needed here.
std::map<int, std::unique_ptr<PreviewServer>> previewServers;

ToolResult previewError(const std::string& message)
{
  ToolResult result;
  result.isError = true;
  result.text = message;
  return result;
}

#endif // ENABLE_WEBSOCKET

} // anonymous namespace

void register_misc_tools(std::vector<McpTool>& tools)
{
  tools.push_back(LuaTool(
    "run_lua_script",
    "Execute arbitrary Aseprite Lua code (escape hatch). When filename is given, the file is "
    "opened first as app.activeSprite. print() is the only way to return data; changes are not "
    "saved automatically (call spr:saveAs(spr.filename) if you modified the sprite). "
    "WARNING: runs unrestricted code on the host.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "script", json11::Json::object {
            { "type", "string" },
            { "description", "Lua source code to execute" } } },
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Optional Aseprite file to open before running" } } },
      } },
      { "required", json11::Json::array { "script" } },
    },
    true,
    [](const json11::Json& args) {
      const std::string script = argString(args, "script");
      if (script.empty())
        return std::string("print(\"ERROR:Script cannot be empty\")");
      return script;
    }));

  tools.push_back(LuaTool(
    "copy_layers_between_sprites",
    "Copy layers by name, with all their cels, from one .aseprite file to another. "
    "Missing layers are created in the target; with replace existing target cels are "
    "overwritten, and with create_missing_frames the target gains the source's frames. "
    "Target layers that already exist but were never copied keep their content unless "
    "replace is on. Layer names missing from the source are reported and skipped; the "
    "target file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "source_filename", json11::Json::object {
            { "type", "string" },
            { "description", "Source .aseprite file with the layers to copy" } } },
        { "target_filename", json11::Json::object {
            { "type", "string" },
            { "description", "Target .aseprite file to copy the layers into" } } },
        { "layer_names", json11::Json::object {
            { "type", "array" },
            { "description", "Layer names to copy, searched inside groups too" },
            { "items", json11::Json::object { { "type", "string" } } } } },
        { "replace", json11::Json::object {
            { "type", "boolean" },
            { "description", "Overwrite existing cels in target layers (default: true)" } } },
        { "create_missing_frames", json11::Json::object {
            { "type", "boolean" },
            { "description", "Add frames to the target when it has fewer than the source "
                             "(default: true)" } } },
      } },
      { "required", json11::Json::array {
        "source_filename", "target_filename", "layer_names" } },
    },
    false,
    [](const json11::Json& args) -> std::string {
      const std::string sourceFilename = argString(args, "source_filename");
      const std::string targetFilename = argString(args, "target_filename");
      // Canonical identity, so symlinked spellings of one file cannot be
      // opened as two different sprites and clobber each other.
      const std::string absSource = canonicalFilePath(sourceFilename);
      const std::string absTarget = canonicalFilePath(targetFilename);
      if (absSource == absTarget)
        return luaError("source and target must be different files");
      if (!base::is_file(absSource))
        return luaError("File " + sourceFilename + " not found");
      if (!base::is_file(absTarget))
        return luaError("File " + targetFilename + " not found");

      const json11::Json& layerNames = args["layer_names"];
      std::string names;
      for (const json11::Json& name : layerNames.array_items()) {
        if (!name.is_string())
          continue;
        if (!names.empty())
          names += ", ";
        names += "\"" + luaString(name.string_value()) + "\"";
      }
      if (names.empty())
        return luaError("Layer names list cannot be empty");

      const std::string replace = argBool(args, "replace", true) ? "true" : "false";
      const std::string createFrames =
        argBool(args, "create_missing_frames", true) ? "true" : "false";

      std::string script =
        "local src = app.open(\"" + luaString(absSource) + "\")\n"
        "if not src then print(\"ERROR:Source sprite not opened\") return end\n"
        "local dst = app.open(\"" + luaString(absTarget) + "\")\n"
        "if not dst then print(\"ERROR:Target sprite not opened\") return end\n"
        "\n" +
        std::string(kFindLayerLua) +
        "\n"
        "local names = {" + names + "}\n"
        "local valid, missing = {}, {}\n"
        "for _, name in ipairs(names) do\n"
        "  if find_layer(src, name) then table.insert(valid, name)\n"
        "  else table.insert(missing, name) end\n"
        "end\n"
        "if #valid == 0 then\n"
        "  print(\"ERROR:None of the requested layers exist in the source: \" ..\n"
        "        table.concat(missing, \", \"))\n"
        "  return\n"
        "end\n"
        "\n"
        "app.transaction(function()\n"
        "  if " + createFrames + " then\n"
        "    while #dst.frames < #src.frames do dst:newFrame() end\n"
        "  end\n"
        "\n"
        "  for _, name in ipairs(valid) do\n"
        "    local src_layer = find_layer(src, name)\n"
        "    local dst_layer = find_layer(dst, name)\n"
        "    if not dst_layer then\n"
        "      dst_layer = dst:newLayer()\n"
        "      dst_layer.name = name\n"
        "    end\n"
        "    if " + replace + " then\n"
        "      for i = 1, #dst.frames do\n"
        "        local cel = dst_layer:cel(dst.frames[i])\n"
        "        if cel then dst:deleteCel(cel) end\n"
        "      end\n"
        "    end\n"
        "    for i = 1, #src.frames do\n"
        "      if i <= #dst.frames then\n"
        "        local src_cel = src_layer:cel(src.frames[i])\n"
        "        if src_cel then\n"
        "          local dst_cel = dst_layer:cel(dst.frames[i])\n"
        "          if dst_cel and " + replace + " then\n"
        "            dst:deleteCel(dst_cel)\n"
        "            dst_cel = nil\n"
        "          end\n"
        "          if not dst_cel then\n"
        "            local img = src_cel.image:clone()\n"
        "            dst:newCel(dst_layer, dst.frames[i], img, src_cel.position)\n"
        "          end\n"
        "        end\n"
        "      end\n"
        "    end\n"
        "  end\n"
        "end)\n"
        "\n"
        "dst:saveAs(dst.filename)\n"
        "if #missing > 0 then\n"
        "  print(\"MISSING:\" .. table.concat(missing, \", \"))\n"
        "end\n"
        "print(\"OK\")";
      return script;
    }));

  tools.push_back(NativeTool(
    "animation_workflow_guide",
    "Return a short guide to the animation workflow these tools support: planning the "
    "palette first, building in layers, drawing coarse to fine, checking the work with "
    "exported frames, shading, animating with the cel tools, verifying, and exporting.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "use_case", json11::Json::object {
            { "type", "string" },
            { "description", "Label for the workflow context (default: \"character\")" } } },
      } },
      { "required", json11::Json::array {} },
    },
    [](const json11::Json& args) -> ToolResult {
      std::string useCase = argString(args, "use_case", "character");
      if (useCase.empty())
        useCase = "character";

      ToolResult result;
      result.text =
        "Animation workflow guide\n"
        "Use case: " + useCase + "\n"
        "\n"
        "1. Plan the palette first: pick a small, coherent set of colors with set_palette, "
        "apply_palette_preset or extract_palette before drawing, so every frame stays "
        "consistent.\n"
        "2. Build in layers: one layer per body part or scene element with add_layer; keep "
        "static parts separate from moving ones so they can be propagated instead of "
        "redrawn.\n"
        "3. Draw coarse to fine: block in the big shapes with the drawing tools first, then "
        "refine silhouettes and details in later passes.\n"
        "4. Look at your work: export_frame at 8-10x scale and inspect the returned image "
        "between passes instead of guessing at pixel level.\n"
        "5. Shade with intent: shade on its own layer and reach for adjust_hsl_native, "
        "adjust_brightness_contrast or apply_convolution for engine-quality touches.\n"
        "6. Animate with the cel tools: block key poses on the first frame, then copy "
        "cels/frames, propagate_cels for static layers, and tween or offset cel positions "
        "for motion.\n"
        "7. Verify motion: render_onion_skin shows neighboring frames together, and "
        "compare_frames reports what actually changed between two frames.\n"
        "8. Export and inspect: tag loop ranges with set_tag, export with export_tag, "
        "export_spritesheet or export_sprite, and check the result before calling it "
        "done.";
      return result;
    }));

#ifdef ENABLE_WEBSOCKET
  tools.push_back(NativeTool(
    "start_preview_server",
    "Start a small HTTP server, bound to 127.0.0.1 only, that serves the files of a "
    "directory (e.g. exported sprites) so they can be inspected in a browser. At most "
    "one server per port; stop it with stop_preview_server. The server lives inside "
    "Aseprite, so it stops when Aseprite exits.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "directory", json11::Json::object {
            { "type", "string" },
            { "description", "Directory to serve" } } },
        { "port", json11::Json::object {
            { "type", "integer" },
            { "description", "TCP port to bind (default: 8000)" } } },
      } },
      { "required", json11::Json::array { "directory" } },
    },
    [](const json11::Json& args) -> ToolResult {
      const std::string directory = argString(args, "directory");
      const int port = argInt(args, "port", 8000);
      if (port < 1 || port > 65535)
        return previewError("Port must be between 1 and 65535");

      const std::string absDirectory = base::get_absolute_path(directory);
      if (!base::is_directory(absDirectory))
        return previewError("Directory not found: " + directory);
      if (previewServers.count(port))
        return previewError("Preview server already running on port " + std::to_string(port));

      std::unique_ptr<PreviewServer> server =
        std::make_unique<PreviewServer>(port, absDirectory);
      if (!server->isRunning())
        return previewError("Cannot start preview server on port " + std::to_string(port) +
                            ": " + server->error());
      previewServers[port] = std::move(server);

      ToolResult result;
      result.text = "Preview server started: http://127.0.0.1:" + std::to_string(port) + "/";
      return result;
    }));

  tools.push_back(NativeTool(
    "stop_preview_server",
    "Stop a preview HTTP server started with start_preview_server. The port must match "
    "the one used to start it.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "port", json11::Json::object {
            { "type", "integer" },
            { "description", "TCP port of the server to stop (default: 8000)" } } },
      } },
      { "required", json11::Json::array {} },
    },
    [](const json11::Json& args) -> ToolResult {
      const int port = argInt(args, "port", 8000);
      const auto it = previewServers.find(port);
      if (it == previewServers.end())
        return previewError("No preview server running on port " + std::to_string(port));

      it->second->stop();
      previewServers.erase(it);

      ToolResult result;
      result.text = "Preview server stopped on port " + std::to_string(port);
      return result;
    }));
#endif // ENABLE_WEBSOCKET
}

} // namespace mcp
} // namespace app
