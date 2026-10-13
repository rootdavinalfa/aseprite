// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/mcp/mcp_tool_context.h"

#include "app/commands/commands.h"
#include "app/commands/params.h"
#include "app/context.h"
#include "app/doc.h"
#include "app/doc_access.h"
#include "app/script/engine.h"
#include "base/fs.h"
#include "base/time.h"

namespace app {
namespace mcp {

namespace {

const char kErrorPrefix[] = "ERROR:";

// Collects everything the engine prints while a snippet runs.
class EngineCapture : public script::EngineDelegate {
public:
  void onConsoleError(const char* text) override { append(text); }
  void onConsolePrint(const char* text) override { append(text); }

  std::string text;

private:
  void append(const char* text)
  {
    if (!text)
      return;
    if (!this->text.empty())
      this->text += "\n";
    this->text += text;
  }
};

} // anonymous namespace

ToolContext& ToolContext::instance()
{
  static ToolContext context;
  return context;
}

bool ToolContext::openDocument(const std::string& filename, std::string& error)
{
  if (!m_context) {
    error = "MCP tool context not initialized";
    return false;
  }

  const std::string absFn = base::get_absolute_path(filename);
  if (!base::is_file(absFn)) {
    error = "File " + filename + " not found";
    return false;
  }
  const base::Time fileTime = base::get_modification_time(absFn);
  const size_t fileSize = base::file_size(absFn);

  // A document left open by an earlier tool call is reused as-is when
  // it has unsaved changes; otherwise it is only reusable when its
  // file hasn't changed on disk (a stale document is dropped so the
  // reload below doesn't resurrect it). The size is compared too
  // because the modification time is only second-granular.
  if (Doc* doc = m_context->documents().getByFileName(absFn)) {
    if (doc->isModified() ||
        (m_cachedFilename == absFn && m_cachedTime && *m_cachedTime == fileTime &&
         m_cachedSize == fileSize)) {
      m_context->setActiveDocument(doc);
      return true;
    }

    m_cachedFilename.clear();
    m_cachedTime.reset();

    try {
      DocDestroyer destroyer(m_context, doc, 500);
      destroyer.destroyDocument();
    }
    catch (const LockedDocException&) {
      // Cannot drop it while another operation holds it; return the
      // stale document rather than failing the whole tool call.
      m_context->setActiveDocument(doc);
      return true;
    }
  }

  Command* openCommand = Commands::instance()->byId(CommandId::OpenFile());
  Params params;
  params.set("filename", absFn.c_str());
  m_context->executeCommand(openCommand, params);

  Doc* doc = m_context->activeDocument();
  if (!doc || base::normalize_path(doc->filename()) != base::normalize_path(absFn)) {
    error = "Failed to open " + filename;
    return false;
  }

  m_cachedFilename = absFn;
  m_cachedTime = fileTime;
  m_cachedSize = fileSize;
  return true;
}

ToolResult ToolContext::runLua(const std::string& code)
{
  ToolResult result;

  if (!m_engine) {
    result.isError = true;
    result.text = "MCP tool context not initialized";
    return result;
  }

  std::lock_guard<std::mutex> lock(m_mutex);

  EngineCapture capture;
  script::ScopedEngineDelegate delegate(m_engine, &capture);
  const bool ok = m_engine->evalCode(code, "mcp:tool");

  if (!ok) {
    result.isError = true;
    result.text = capture.text;
  }
  else if (capture.text.compare(0, sizeof(kErrorPrefix) - 1, kErrorPrefix) == 0) {
    result.isError = true;
    result.text = capture.text.substr(sizeof(kErrorPrefix) - 1);
  }
  else {
    result.text = capture.text;
  }
  return result;
}

} // namespace mcp
} // namespace app
