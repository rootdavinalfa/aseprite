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
#include "app/site.h"
#include "base/fs.h"
#include "base/time.h"
#include "doc/layer.h"
#include "doc/sprite.h"

#include <algorithm>
#include <vector>

namespace app {
namespace mcp {

namespace {

const char kErrorPrefix[] = "ERROR:";

// Index chain from the sprite root down to "layer" (empty when the
// layer doesn't belong to the sprite). Indices survive a document
// reload, unlike object ids.
std::vector<int> layerPath(const doc::Sprite* sprite, const doc::Layer* layer)
{
  std::vector<int> path;
  if (!layer || layer->sprite() != sprite)
    return path;

  while (layer && layer->parent()) {
    const doc::LayerList& siblings = layer->parent()->layers();
    const auto it = std::find(siblings.begin(), siblings.end(), layer);
    if (it == siblings.end())
      return {};
    path.push_back(int(it - siblings.begin()));
    layer = layer->parent();
  }
  std::reverse(path.begin(), path.end());
  return path;
}

doc::Layer* layerAtPath(const doc::Sprite* sprite, const std::vector<int>& path)
{
  const doc::LayerList* layers = &sprite->root()->layers();
  doc::Layer* layer = nullptr;
  for (const int index : path) {
    if (index < 0 || index >= int(layers->size()))
      return nullptr;
    layer = (*layers)[index];
    layers = &layer->layers();
  }
  return layer;
}

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

  // Canonical identity: alternate spellings of the same file (symlinks,
  // /tmp vs /private/tmp) must map to a single document/cache entry.
  const std::string absFn = canonicalFilePath(filename);
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
  std::vector<int> activeLayerPath;
  if (Doc* doc = m_context->documents().getByFileName(absFn)) {
    if (doc->isModified() ||
        (m_cachedFilename == absFn && m_cachedTime && *m_cachedTime == fileTime &&
         m_cachedSize == fileSize)) {
      m_context->setActiveDocument(doc);
      return true;
    }

    // Reopening the file below creates a new document, whose active
    // layer would default back to the bottom one. Remember the layer
    // chosen in this session (set_layer) so the reload keeps it.
    activeLayerPath = layerPath(doc->sprite(), m_context->activeSite().layer());

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

  if (!activeLayerPath.empty()) {
    if (doc::Layer* layer = layerAtPath(doc->sprite(), activeLayerPath))
      m_context->setActiveLayer(layer);
  }

  m_cachedFilename = absFn;
  m_cachedTime = fileTime;
  m_cachedSize = fileSize;
  return true;
}

void ToolContext::refreshCachedStat()
{
  if (!m_context || m_cachedFilename.empty() || !base::is_file(m_cachedFilename))
    return;

  // Only refresh a document with no unsaved changes: it is reused
  // regardless of the file on disk, while the stat of an unmodified
  // one is compared on the next call (external changes must still
  // invalidate the cache).
  Doc* doc = m_context->documents().getByFileName(m_cachedFilename);
  if (!doc || doc->isModified())
    return;

  m_cachedTime = base::get_modification_time(m_cachedFilename);
  m_cachedSize = base::file_size(m_cachedFilename);
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
