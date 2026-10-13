// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/mcp/mcp_tools_lua.h"
#include "app/mcp/mcp_tools_registry.h"

#include "base/fs.h"

#include <string>

namespace app {
namespace mcp {

namespace {

// A snippet that reports a validation failure; the "ERROR:" prefix
// turns the tool result into an error.
std::string luaError(const std::string& message)
{
  return "print(\"ERROR:" + luaString(message) + "\")";
}

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
      const std::string absSource = base::get_absolute_path(sourceFilename);
      const std::string absTarget = base::get_absolute_path(targetFilename);
      if (base::normalize_path(absSource) == base::normalize_path(absTarget))
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
}

} // namespace mcp
} // namespace app
