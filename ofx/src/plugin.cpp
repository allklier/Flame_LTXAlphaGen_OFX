// LTX Alpha Gen OFX — PROTOTYPE round 3 (CLAUDE.md decisions 35–43).
// - Render never runs the model: it serves mattes from the disk cache, else a faint
//   "no media"-style marker (Flame calls render on its UI thread — Flame test 1).
// - Process stages the clip's frames in /dev/shm and launches a detached job
//   (worker/job.py) with its own progress window and Cancel button.
// - A watcher thread follows the job and nudges Flame to re-render (decision 42, option a:
//   setting params outside an action breaks the OFX threading rules — experiment, toggleable).
// - Layout follows Flame's observed rules: one group = one column of <= 5 rows.
// - Licence + model gating (bypassable only in dev builds). Logging per decision 39.
// Not in the prototype: OCIO (colour space only feeds the cache key), real model, resident worker.

#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

#include "cache.h"
#include "chunk_plan.h"
#include "env_info.h"
#include "half.h"
#include "log.h"
#include "marker.h"
#include "nvml.h"
#include "ofxsImageEffect.h"
#include "process.h"
#include "transport.h"
#include "user_config.h"

#ifndef LTXAG_DEV_BUILD
#define LTXAG_DEV_BUILD 0
#endif

namespace {

using namespace ltxag;

constexpr const char* kPluginId = "com.janklier.ltx.alphagen";
constexpr const char* kPluginLabel = "LTX Alpha Gen";
// Part of every cache key: which worker/model produced a matte (dummy and real never mix).
constexpr const char* kModelVersion = "ltx2.5-alphagen0.9-recipe1";
constexpr const char* kDummyVersion = "dummy-0.3";

// Placeholder until build step 3 measures real peak VRAM (decision 34).
constexpr double kPlaceholderBf16NeedGB = 58.8;  // measured: HD 1920x1088, 145 frames, bf16 (build step 3)

const char* const kColourSpaces[] = {"Bypass", "ACEScg", "LogC4", "Log3G10"};
const char* const kPrecisions[] = {"bf16", "fp8"};
const char* const kSimulations[] = {"none", "oom", "crash", "hang"};

// Params. Column (group) in brackets; see CLAUDE.md "Revised layout".
// [Alpha Gen]
constexpr const char* kPColour = "colourSpace";
constexpr const char* kPProcess = "process";
constexpr const char* kPStatus = "status";
constexpr const char* kPShowMarker = "showMarker";
// [Performance]
constexpr const char* kPPrecision = "precision";
constexpr const char* kPUnload = "unloadAfterRun";
constexpr const char* kPHeadroom = "headroomGB";
constexpr const char* kPSaveDefault = "saveAsDefault";
// [Licence] — per user: not saved in setups, mirrored to the user config.
constexpr const char* kPViewLicence = "viewLicence";
constexpr const char* kPAccept = "acceptLicence";
constexpr const char* kPTokenCaption = "tokenCaption";
constexpr const char* kPToken = "hfToken";
constexpr const char* kPDownload = "download";
// [Models] — per user.
constexpr const char* kPModelDir = "modelDir";
constexpr const char* kPSetupStatus = "setupStatus";
constexpr const char* kPDebugLog = "debugLogging";
constexpr const char* kPCollectLogs = "collectLogs";
// [Cache] — per user, except Purge Cache (this node).
constexpr const char* kPCacheDir = "cacheDir";
constexpr const char* kPCacheInfo = "cacheInfo";
constexpr const char* kPPurgeNode = "purgeCache";
constexpr const char* kPPurgeAll = "purgeAll";
// [Prototype] — dev builds only.
constexpr const char* kPSimSeconds = "simSeconds";
constexpr const char* kPSimulate = "simulate";
constexpr const char* kPSimNeed = "simNeedGB";
constexpr const char* kPPython = "pythonPath";
constexpr const char* kPEnforceGate = "enforceGate";
constexpr const char* kPAutoRefresh = "autoRefresh";
constexpr const char* kPUseDummy = "useDummy";
// Hidden.
constexpr const char* kPInitialized = "initialized";
constexpr const char* kPNodeId = "nodeId";
constexpr const char* kPRefreshTick = "refreshTick";

constexpr const char* kTokenCaptionDefault = "Hugging Face token:";

std::string resourcesDir() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&resourcesDir), &info) || !info.dli_fname) return "";
  std::string p = info.dli_fname;  // .../Contents/Linux-x86-64/LTXAlphaGen.ofx
  p = p.substr(0, p.rfind('/'));
  p = p.substr(0, p.rfind('/'));
  return p + "/Resources";
}

std::string homeDir() {
  const char* h = std::getenv("HOME");
  return h ? h : "/tmp";
}

std::string timeStr(const char* fmt) {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char b[32];
  std::strftime(b, sizeof b, fmt, &tm);
  return b;
}

std::string fmtGB(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.1f", v);
  return b;
}

uint64_t fnv(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ULL;
  return h;
}

std::string randomId() {
  std::random_device rd;
  char b[9];
  std::snprintf(b, sizeof b, "%08x", rd());
  return b;
}

// Value of "key" in the job status JSON written by Python's json.dump.
std::string jsonField(const std::string& j, const std::string& key) {
  const std::string k = "\"" + key + "\": ";
  const auto p = j.find(k);
  if (p == std::string::npos) return "";
  const size_t b = p + k.size();
  if (b < j.size() && j[b] == '"') return j.substr(b + 1, j.find('"', b + 1) - b - 1);
  const auto e = j.find_first_of(",}", b);
  return j.substr(b, e - b);
}

std::string readFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Licence text shown by "View Licence"; its hash is stored on acceptance (decision 38).
std::string licencePath() { return resourcesDir() + "/licence.txt"; }
std::string licenceHash() {
  char b[17];
  std::snprintf(b, sizeof b, "%016llx",
                static_cast<unsigned long long>(fnv(readFile(licencePath()))));
  return b;
}

struct SetupState {
  bool licenceAccepted = false;
  bool modelsReady = false;
  bool ready() const { return licenceAccepted && modelsReady; }
};

SetupState setupState() {
  const UserConfig cfg = loadUserConfig();
  SetupState s;
  s.licenceAccepted = cfg.count("licence_accepted") && cfg.count("licence_hash") &&
                      cfg.at("licence_hash") == licenceHash();
  s.modelsReady = cfg.count("models_ready") > 0;
  return s;
}

// Built-in defaults, overridden by the user config (decisions 21, 31, 32, 34).
struct Defaults {
  int precision = 0;
  double headroomGB = 0.0;
  bool unload = true;
  std::string source;
};

Defaults computeDefaults() {
  Defaults d;
  const VramInfo v = queryVram();
  d.headroomGB = v.ok ? std::round(v.freeGB * 0.05 * 10.0) / 10.0 : 2.0;
  d.precision = (v.ok && v.freeGB - d.headroomGB < kPlaceholderBf16NeedGB) ? 1 : 0;
  std::string src = v.ok ? "built-in (free " + fmtGB(v.freeGB) + " of " + fmtGB(v.totalGB) + " GB)"
                         : "built-in (NVML unavailable)";
  const UserConfig cfg = loadUserConfig();
  if (cfg.count("precision")) d.precision = cfg.at("precision") == "fp8" ? 1 : 0;
  if (cfg.count("headroom_gb")) d.headroomGB = std::atof(cfg.at("headroom_gb").c_str());
  if (cfg.count("unload")) d.unload = cfg.at("unload") == "1";
  if (cfg.count("precision") || cfg.count("headroom_gb") || cfg.count("unload"))
    src += " + user config";
  d.source = src;
  return d;
}

uint64_t hashImage(const OFX::Image& img) {
  const OfxRectI b = img.getBounds();
  const size_t rowBytes = size_t(b.x2 - b.x1) * 4 *
                          (img.getPixelComponents() == OFX::ePixelComponentRGBA ? 4 : 3);
  uint64_t h = 1469598103934665603ULL;
  for (int y = b.y1; y < b.y2; ++y) {
    const auto* row = static_cast<const unsigned char*>(img.getPixelAddress(b.x1, y));
    size_t i = 0;
    for (; i + 8 <= rowBytes; i += 8) {
      uint64_t w;
      std::memcpy(&w, row + i, 8);
      h = (h ^ w) * 1099511628211ULL;
    }
    for (; i < rowBytes; ++i) h = (h ^ row[i]) * 1099511628211ULL;
  }
  return h;
}

std::string jsonStr(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

HostInfo hostInfo() {
  HostInfo h;
  if (const auto* d = OFX::getImageEffectHostDescription()) {
    h.name = d->hostName;
    h.label = d->hostLabel;
    h.version = d->versionLabel + " (" + std::to_string(d->versionMajor) + "." +
                std::to_string(d->versionMinor) + "." + std::to_string(d->versionMicro) + ")";
    h.background = d->hostIsBackground;
  }
  return h;
}

std::atomic<unsigned> gJobSeq{0};

class AlphaGenEffect : public OFX::ImageEffect {
 public:
  explicit AlphaGenEffect(OfxImageEffectHandle handle) : ImageEffect(handle) {
    src_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
    dst_ = fetchClip(kOfxImageEffectOutputClipName);
    colour_ = fetchChoiceParam(kPColour);
    status_ = fetchStringParam(kPStatus);
    showMarker_ = fetchBooleanParam(kPShowMarker);
    precision_ = fetchChoiceParam(kPPrecision);
    unload_ = fetchBooleanParam(kPUnload);
    headroom_ = fetchDoubleParam(kPHeadroom);
    accept_ = fetchBooleanParam(kPAccept);
    tokenCaption_ = fetchStringParam(kPTokenCaption);
    token_ = fetchStringParam(kPToken);
    modelDir_ = fetchStringParam(kPModelDir);
    setupStatus_ = fetchStringParam(kPSetupStatus);
    debugLog_ = fetchBooleanParam(kPDebugLog);
    cacheDir_ = fetchStringParam(kPCacheDir);
    cacheInfo_ = fetchStringParam(kPCacheInfo);
    simSeconds_ = fetchDoubleParam(kPSimSeconds);
    simulate_ = fetchChoiceParam(kPSimulate);
    simNeed_ = fetchDoubleParam(kPSimNeed);
    python_ = fetchStringParam(kPPython);
    enforceGate_ = fetchBooleanParam(kPEnforceGate);
    autoRefresh_ = fetchBooleanParam(kPAutoRefresh);
    useDummy_ = fetchBooleanParam(kPUseDummy);
    initialized_ = fetchBooleanParam(kPInitialized);
    nodeIdParam_ = fetchStringParam(kPNodeId);
    refreshTick_ = fetchIntParam(kPRefreshTick);

    try {
      nodeIdParam_->getValue(nodeId_);
      if (nodeId_.empty()) {
        nodeId_ = randomId();  // saved with the setup; copies of a node share it
        nodeIdParam_->setValue(nodeId_);
      }
      LogScope scope(nodeId_);
      bool init = false;
      initialized_->getValue(init);
      LOGI("createInstance %p: initialized=%d", static_cast<void*>(this), int(init));
      if (!init) {
        // New node (decision 21): take the current user defaults; a node loaded from a
        // saved setup keeps its own values because it carries initialized=true.
        const Defaults d = computeDefaults();
        precision_->setValue(d.precision);
        headroom_->setValue(d.headroomGB);
        unload_->setValue(d.unload);
        initialized_->setValue(true);
        LOGI("new node: defaults [%s] precision=%s headroom=%.1f unload=%d", d.source.c_str(),
             kPrecisions[d.precision], d.headroomGB, int(d.unload));
      }
      // Per-user values are not stored in setups; mirror them from the user config.
      const UserConfig cfg = loadUserConfig();
      accept_->setValue(setupState().licenceAccepted);
      if (cfg.count("model_dir")) modelDir_->setValue(cfg.at("model_dir"));
      cacheDir_->setValue(cacheSettings(cfg).root);
      debugLog_->setValue(logLevel() == LogLevel::Debug);
      refreshSetupUi();
      refreshCacheInfo();
    } catch (const std::exception& e) {
      LOGE("createInstance: setting param values FAILED: %s [%s]", e.what(), typeid(e).name());
    }
  }

  ~AlphaGenEffect() override {
    LogScope scope(nodeId_);
    stopWatcher();
    LOGI("destroyInstance %p", static_cast<void*>(this));
  }

  void getClipPreferences(OFX::ClipPreferencesSetter& prefs) override {
    // Decision 43: ask for a matte-only output. Flame ignores this (test 3: output stays RGBA),
    // so render() also handles RGBA (decision 48). Kept: other hosts may honour it.
    prefs.setClipComponents(*dst_, OFX::ePixelComponentAlpha);
    LOGD("getClipPreferences: output set to Alpha");
  }

  void changedParam(const OFX::InstanceChangedArgs& args, const std::string& name) override {
    // Flame reports our own setValue() calls back synchronously as changedParam (reason =
    // plugin edit); reacting to those recursed until Flame crashed (Flame test 2). Only user
    // edits are handled, and never re-entrantly.
    if (args.reason != OFX::eChangeUserEdit || inChangedParam_) return;
    struct Guard {
      bool& b;
      explicit Guard(bool& b) : b(b) { b = true; }
      ~Guard() { b = false; }
    } guard(inChangedParam_);
    LogScope scope(nodeId_);
    LOGI("changedParam %s (user) time=%g", name.c_str(), args.time);
    try {
      handleParam(name, args.time);
    } catch (const std::exception& e) {
      LOGE("changedParam %s FAILED: %s [%s]", name.c_str(), e.what(), typeid(e).name());
      throw;
    }
    refreshSetupUi();
  }

  void render(const OFX::RenderArguments& args) override {
    LogScope scope(nodeId_);
    LOGD("render t=%g interactive=%d sequential=%d window=(%d,%d)-(%d,%d)", args.time,
         int(args.interactiveRenderStatus), int(args.sequentialRenderStatus),
         args.renderWindow.x1, args.renderWindow.y1, args.renderWindow.x2, args.renderWindow.y2);
    std::unique_ptr<OFX::Image> out(dst_->fetchImage(args.time));
    if (!out) {
      LOGE("render t=%g: host returned no output image", args.time);
      OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    std::vector<float> matte;
    std::string message, caption;
    const OfxRectI db = out->getBounds();
    OfxRectI sb = db;
    std::unique_ptr<OFX::Image> src;

    if (gateClosed()) {
      message = "Setup required: see the Licence and Models columns.";
      caption = "SETUP REQUIRED";
    } else {
      src.reset(src_->fetchImage(args.time));
      if (!src) {
        message = "No source image (is the input connected?).";
        caption = "NOT CONNECTED";
      } else if (src->getPixelDepth() != OFX::eBitDepthFloat) {
        message = "Only 32-bit float images are supported.";
        caption = "NOT SUPPORTED";
      } else {
        sb = src->getBounds();
        const int w = sb.x2 - sb.x1, h = sb.y2 - sb.y1;
        const std::string key = matteKey(hashImage(*src), paramSignature());
        std::vector<uint16_t> half;
        if (readMatte(cacheRoot(), key, w, h, half)) {
          matte.resize(half.size());
          halfToFloat(half.data(), matte.data(), matte.size());
        } else if (isPending(cacheRoot(), key)) {
          message = "Processing: this frame is not ready yet.";
          caption = "PROCESSING";
        } else {
          const std::string status = lastJobStatus();
          const std::string state = jsonField(status, "state");
          if (state == "failed") {
            message = "Processing failed: " + jsonField(status, "message") +
                      ". Press Process to try again.";
            caption = "FAILED";
          } else if (state == "cancelled") {
            message = "Processing was cancelled. Press Process to try again.";
            caption = "CANCELLED";
          } else {
            message = "Not processed: press Process.";
            caption = "NOT PROCESSED";
          }
        }
        LOGD("render t=%g key=%s %s", args.time, key.c_str(),
             matte.empty() ? caption.c_str() : "cache hit");
      }
    }

    const int w = sb.x2 - sb.x1, h = sb.y2 - sb.y1;  // source frame size (the matte's size)
    if (matte.empty()) {
      bool marker = true;
      showMarker_->getValue(marker);
      if (marker) drawMarker(matte, w, h, caption);
    }
    // Flame always gives an RGBA output and shows it as an image output plus a matte output
    // (decision 43 attempt failed). Decision 48: the image mirrors the source, and the alpha
    // (Flame's matte output) carries the matte or marker. A 1-channel output gets the matte.
    const bool rgbFromSource = src && src->getPixelDepth() == OFX::eBitDepthFloat;
    const OfxRectI& win = args.renderWindow;
    const int comps = dst_->getPixelComponents() == OFX::ePixelComponentRGBA  ? 4
                      : dst_->getPixelComponents() == OFX::ePixelComponentRGB ? 3
                                                                               : 1;
    if (!loggedOutput_) {
      LOGI("render: output components %s (%d channel(s))",
           dst_->getPixelComponentsProperty().c_str(), comps);
      loggedOutput_ = true;
    }
    for (int y = std::max(win.y1, db.y1); y < std::min(win.y2, db.y2); ++y) {
      auto* px = static_cast<float*>(out->getPixelAddress(std::max(win.x1, db.x1), y));
      const int row = (h - 1) - (y - sb.y1);  // OFX is bottom-up, the matte top-down
      for (int x = std::max(win.x1, db.x1); x < std::min(win.x2, db.x2); ++x) {
        const int col = x - sb.x1;
        const bool inside = row >= 0 && row < h && col >= 0 && col < w;
        const float v = (!matte.empty() && inside) ? matte[size_t(row) * w + col] : 0.0f;
        if (comps == 1) {
          *px++ = v;
          continue;
        }
        const float* sp = (rgbFromSource && inside)
                              ? static_cast<const float*>(src->getPixelAddress(x, y))
                              : nullptr;
        for (int k = 0; k < 3; ++k) *px++ = sp ? sp[k] : 0.0f;
        if (comps == 4) *px++ = v;
      }
    }
    try {
      if (caption.empty())
        clearPersistentMessage();
      else
        setPersistentMessage(OFX::Message::eMessageWarning, "", message);
    } catch (const std::exception& e) {
      LOGW("render: persistent message FAILED: %s", e.what());
    }
  }

 private:
  void handleParam(const std::string& name, double time) {
    if (name == kPProcess) {
      process(time);
    } else if (name == kPPurgeNode) {
      purgeNode();
    } else if (name == kPSaveDefault) {
      saveAsDefault();
    } else if (name == kPViewLicence) {
      std::string err;
      const bool ok = launchDetached({"/usr/bin/zenity", "--text-info", "--title=LTX-2.x Licence",
                                      "--filename=" + licencePath(), "--width=640", "--height=520"},
                                     cleanEnvironment(), err);
      if (ok)
        LOGI("viewLicence: window opened");
      else
        LOGE("viewLicence: FAILED %s", err.c_str());
    } else if (name == kPAccept) {
      bool v = false;
      accept_->getValue(v);
      UserConfig cfg = loadUserConfig();
      if (v) {
        cfg["licence_accepted"] = timeStr("%Y-%m-%d");
        cfg["licence_hash"] = licenceHash();
      } else {
        cfg.erase("licence_accepted");
        cfg.erase("licence_hash");
      }
      saveUserConfig(cfg);
      LOGI("licence %s (hash %s)", v ? "accepted" : "withdrawn", licenceHash().c_str());
    } else if (name == kPModelDir || name == kPCacheDir) {
      std::string v;
      (name == kPModelDir ? modelDir_ : cacheDir_)->getValue(v);
      UserConfig cfg = loadUserConfig();
      cfg[name == kPModelDir ? "model_dir" : "cache_dir"] = v;
      saveUserConfig(cfg);
      LOGI("%s set to %s", name.c_str(), v.c_str());
      refreshCacheInfo();
    } else if (name == kPDebugLog) {
      bool v = false;
      debugLog_->getValue(v);
      UserConfig cfg = loadUserConfig();
      cfg["log_level"] = v ? "debug" : "info";
      saveUserConfig(cfg);
      setLogLevel(v ? LogLevel::Debug : LogLevel::Info);
      LOGI("debug logging %s", v ? "on" : "off");
    } else if (name == kPCollectLogs) {
      collectLogs();
    } else if (name == kPDownload) {
      download();
    } else if (name == kPPurgeAll) {
      const std::string root = cacheRoot();
      const uint64_t freed = purgeAll(root);
      LOGI("purgeAll %s: %.1f MB freed", root.c_str(), freed / 1048576.0);
      refreshCacheInfo();
    }
  }

  bool gateClosed() {
    bool enforce = true;
#if LTXAG_DEV_BUILD
    enforceGate_->getValue(enforce);
#endif
    return enforce && !setupState().ready();
  }

  // Disable everything outside Licence/Models/Cache while setup is incomplete (decision 38).
  void refreshSetupUi() {
    const SetupState st = setupState();
    const bool closed = gateClosed();
    for (const char* p : {kPColour, kPProcess, kPShowMarker, kPPurgeNode, kPPrecision, kPUnload,
                          kPHeadroom, kPSaveDefault}) {
      try {
        getParam(p)->setEnabled(!closed);
      } catch (const std::exception& e) {
        LOGW("setEnabled(%s) FAILED: %s", p, e.what());
      }
    }
    // <= 28 characters: Flame cuts text at ~30 (test 2 screenshots).
    setIfChanged(setupStatus_, std::string("Licence: ") + (st.licenceAccepted ? "OK" : "NO") +
                                   " | Models: " + (st.modelsReady ? "OK" : "NO"));
  }

  // Avoids needless host notifications (each setValue is reported back as a change).
  void setIfChanged(OFX::StringParam* p, const std::string& v) {
    try {
      std::string cur;
      p->getValue(cur);
      if (cur != v) p->setValue(v);
    } catch (const std::exception& e) {
      LOGW("setValue FAILED: %s", e.what());
    }
  }

  std::string cacheRoot() { return cacheSettings(loadUserConfig()).root; }

  void refreshCacheInfo() {
    setIfChanged(cacheInfo_, fmtGB(cacheBytes(cacheRoot()) / 1073741824.0) + " GB used");
  }

  bool useDummy() {
    bool dummy = false;
#if LTXAG_DEV_BUILD
    useDummy_->getValue(dummy);
#endif
    return dummy;
  }

  std::string paramSignature() {
    int colour = 0, prec = 0;
    colour_->getValue(colour);
    precision_->getValue(prec);
    return std::string("v1|cs=") + kColourSpaces[colour] + "|prec=" + kPrecisions[prec] +
           "|model=" + (useDummy() ? kDummyVersion : kModelVersion);
  }

  std::string lastJobStatus() {
    std::string id;
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      id = lastJobId_;
    }
    return id.empty() ? "" : readFile(cacheRoot() + "/jobs/" + id + ".json");
  }

  std::string pythonPath() {
    std::string p;
    python_->getValue(p);
    return p.empty() ? "/usr/bin/python3" : p;
  }

  void error(const std::string& msg) {
    LOGW("error shown to user: %s", msg.c_str());
    sendMessage(OFX::Message::eMessageError, "", msg);
  }

  // Fetches every frame of the clip during this action and hands them to a detached job.
  void process(double time) {
    if (gateClosed()) return error("Complete the Licence and Models setup first.");
    const UserConfig cfg = loadUserConfig();
    std::string modelDir;
    modelDir_->getValue(modelDir);
    const std::string ltxPython = cfg.count("ltx_python") ? cfg.at("ltx_python") : "";
    if (!useDummy()) {
      struct stat st {};
      if (ltxPython.empty() || stat(ltxPython.c_str(), &st) != 0)
        return error("The LTX runtime is not installed (no ltx_python in " + userConfigPath() + ").");
      if (stat((modelDir + "/diffusion_models").c_str(), &st) != 0)
        return error("No models found in the Model Folder: " + modelDir);
    }
    double need = 0.0, head = 0.0;
    simNeed_->getValue(need);
    headroom_->getValue(head);
    if (need > 0.0) {
      const VramInfo v = queryVram();
      LOGI("preflight: need %.1f GB, free %.1f GB, reserve %.1f GB", need, v.freeGB, head);
      if (v.ok && need > v.freeGB - head)
        return error("Not enough GPU memory: this run needs about " + fmtGB(need) + " GB, " +
                     fmtGB(v.freeGB) + " GB is free and " + fmtGB(head) +
                     " GB is reserved for Flame. Try fp8 precision or a lower reserve.");
    }

    const OfxRangeD r = src_->getFrameRange();
    const int first = int(std::lround(r.min));
    const int n = int(std::lround(r.max)) - first + 1;
    const auto chunks = planChunks(n);
    const std::string sig = paramSignature();
    const std::string root = cacheRoot();
    const std::string jobId = std::to_string(getpid()) + "-" + std::to_string(gJobSeq++) + "-" +
                              std::to_string(std::time(nullptr));
    LogScope scope(nodeId_, jobId);
    const std::string input = "/dev/shm/ltxag_job_" + jobId + "_in.bin";
    LOGI("process: t=%g clip [%d,%d] %d frames, %d chunk(s), params %s", time, first,
         first + n - 1, n, int(chunks.size()), sig.c_str());

    std::vector<std::string> keys(n);
    int w = 0, h = 0;
    FrameWriter writer;
    std::string err;
    std::vector<float> rgb;
    std::vector<uint16_t> frame;
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < n; ++f) {
      std::unique_ptr<OFX::Image> img;
      try {
        img.reset(src_->fetchImage(double(first + f)));
      } catch (const std::exception& e) {
        LOGE("process: fetchImage(%d) threw: %s", first + f, e.what());
      }
      if (!img) {
        writer.close();
        unlink(input.c_str());
        return error("Flame did not provide source frame " + std::to_string(first + f) + ".");
      }
      const OfxRectI b = img->getBounds();
      if (f == 0) {
        w = b.x2 - b.x1, h = b.y2 - b.y1;
        LOGI("process: source %dx%d %s depth %d", w, h, img->getPixelComponentsProperty().c_str(),
             int(img->getPixelDepth()));
        if (img->getPixelDepth() != OFX::eBitDepthFloat)
          return error("Only 32-bit float images are supported.");
        if (!writer.open(input, w, h, n, err)) {
          unlink(input.c_str());
          return error(err);
        }
        rgb.resize(size_t(w) * 3);
        frame.resize(size_t(w) * h * 3);
      } else if (b.x2 - b.x1 != w || b.y2 - b.y1 != h) {
        writer.close();
        unlink(input.c_str());
        return error("Source frames differ in size within the clip.");
      }
      const int sc = img->getPixelComponents() == OFX::ePixelComponentRGBA ? 4 : 3;
      for (int row = 0; row < h; ++row) {
        const auto* p = static_cast<const float*>(img->getPixelAddress(b.x1, b.y2 - 1 - row));
        for (int x = 0; x < w; ++x)
          for (int k = 0; k < 3; ++k) rgb[size_t(x) * 3 + k] = p[size_t(x) * sc + k];
        floatToHalf(rgb.data(), frame.data() + size_t(row) * w * 3, rgb.size());
      }
      keys[f] = matteKey(hashImage(*img), sig);
      if (!writer.writeFrame(frame.data(), err)) {
        writer.close();
        unlink(input.c_str());
        return error(err);
      }
    }
    writer.close();
    const double sec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    LOGI("process: staged %d frames in %.2f s (%.1f ms/frame), %.1f MB in %s", n, sec,
         1000.0 * sec / n, double(n) * w * h * 6 / 1048576.0, input.c_str());

    double seconds = 10.0;
    int sim = 0, prec = 0;
    simSeconds_->getValue(seconds);
    simulate_->getValue(sim);
    precision_->getValue(prec);
    std::ostringstream js;
    js << "{\"job_id\":" << jsonStr(jobId) << ",\"node_id\":" << jsonStr(nodeId_)
       << ",\"input\":" << jsonStr(input) << ",\"width\":" << w << ",\"height\":" << h
       << ",\"frames\":" << n << ",\"chunks\":[";
    for (size_t c = 0; c < chunks.size(); ++c)
      js << (c ? "," : "") << "[" << chunks[c].start << "," << chunks[c].len << "]";
    js << "],\"frame_keys\":[";
    for (int f = 0; f < n; ++f) js << (f ? "," : "") << jsonStr(keys[f]);
    js << "],\"contrib\":[";
    for (int f = 0; f < n; ++f) {
      js << (f ? "," : "") << "[";
      const auto contrib = frameContributions(chunks, f);
      for (size_t c = 0; c < contrib.size(); ++c)
        js << (c ? "," : "") << "[" << contrib[c].chunk << "," << contrib[c].weight << "]";
      js << "]";
    }
    js << "],\"cache_root\":" << jsonStr(root)
       << ",\"worker\":"
       << jsonStr(resourcesDir() + (useDummy() ? "/worker/dummy_worker.py" : "/worker/ltx_worker.py"));
    if (!useDummy())
      js << ",\"worker_python\":" << jsonStr(ltxPython) << ",\"worker_args\":[\"--model-dir\","
         << jsonStr(modelDir) << ",\"--embeddings\"," << jsonStr(root + "/prompt-empty.safetensors")
         << "]";
    js
       << ",\"seconds\":" << seconds << ",\"simulate\":" << jsonStr(kSimulations[sim])
       << ",\"precision\":" << jsonStr(kPrecisions[prec])
       << ",\"title\":" << jsonStr("LTX Alpha Gen - " + std::to_string(n) + " frames") << "}";
    const std::string manifest = "/dev/shm/ltxag_job_" + jobId + ".json";
    {
      std::ofstream mf(manifest);
      mf << js.str();
    }
    const std::string level = logLevel() == LogLevel::Debug ? "debug" : "info";
    const bool ok = launchDetached(
        {pythonPath(), "-I", "-B", resourcesDir() + "/worker/job.py", manifest},
        cleanEnvironment({"LTXAG_LOG_LEVEL=" + level}), err);
    if (!ok) {
      unlink(input.c_str());
      unlink(manifest.c_str());
      return error("Could not start processing: " + err);
    }
    {
      std::lock_guard<std::mutex> lock(jobMutex_);
      lastJobId_ = jobId;
    }
    setIfChanged(status_, "Processing " + std::to_string(n) + " frames");
    LOGI("process: job launched (job python %s, worker %s)", pythonPath().c_str(),
         useDummy() ? "dummy" : ltxPython.c_str());
    startWatcher(jobId, root);
  }

  // --- Decision 42 (a): follow the job and nudge Flame to re-render. EXPERIMENT. ---
  void startWatcher(const std::string& jobId, const std::string& root) {
    stopWatcher();
    bool enabled = true;
    autoRefresh_->getValue(enabled);
    if (!enabled) {
      LOGI("auto refresh off: no watcher");
      return;
    }
    stopFlag_ = false;
    const std::string node = nodeId_;
    watcher_ = std::thread([this, jobId, root, node] { watch(jobId, root, node); });
  }

  void stopWatcher() {
    stopFlag_ = true;
    if (watcher_.joinable()) watcher_.join();
  }

  void watch(const std::string& jobId, const std::string& root, const std::string& node) {
    LogScope scope(node, jobId);
    LOGI("watcher started (this thread is not an OFX action thread)");
    std::string lastSeen;
    int tick = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (!stopFlag_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      const std::string j = readFile(root + "/jobs/" + jobId + ".json");
      if (j.empty()) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(60)) {
          LOGW("watcher: job wrote no status within 60 s; giving up");
          return;
        }
        continue;
      }
      const std::string state = jsonField(j, "state");
      const std::string seen = state + "/" + jsonField(j, "chunks_done");
      if (seen == lastSeen) continue;
      lastSeen = seen;
      std::string text;
      if (state == "running")
        text = "Processing: " + jsonField(j, "chunks_done") + "/" + jsonField(j, "chunks") +
               " chunks done";
      else if (state == "done")
        text = "Done: " + jsonField(j, "frames_done") + " frames";
      else if (state == "cancelled")
        text = "Cancelled";
      else
        text = "Failed: " + jsonField(j, "message").substr(0, 20);
      try {
        LOGI("watcher: %s -> status + refresh tick %d from a non-action thread", seen.c_str(),
             tick + 1);
        setIfChanged(status_, text);
        refreshTick_->setValue(++tick);
        LOGI("watcher: host accepted the param changes");
      } catch (const std::exception& e) {
        LOGE("watcher: param change FAILED: %s [%s]", e.what(), typeid(e).name());
        return;
      }
      if (state != "running") break;
    }
    LOGI("watcher finished");
  }

  void purgeNode() {
    const OfxRangeD r = src_->getFrameRange();
    const std::string sig = paramSignature();
    std::vector<std::string> keys;
    for (int t = int(std::lround(r.min)); t <= int(std::lround(r.max)); ++t) {
      std::unique_ptr<OFX::Image> img(src_->fetchImage(double(t)));
      if (img) keys.push_back(matteKey(hashImage(*img), sig));
    }
    const size_t n = purgeKeys(cacheRoot(), keys);
    LOGI("purgeNode: %zu of %zu frames had cached mattes, removed", n, keys.size());
    setIfChanged(status_, "Cache purged: " + std::to_string(n) + " frames");
    refreshCacheInfo();
  }

  void saveAsDefault() {
    int prec = 0;
    double head = 0.0;
    bool unl = true;
    precision_->getValue(prec);
    headroom_->getValue(head);
    unload_->getValue(unl);
    UserConfig cfg = loadUserConfig();
    cfg["precision"] = kPrecisions[prec];
    cfg["headroom_gb"] = fmtGB(head);
    cfg["unload"] = unl ? "1" : "0";
    const bool ok = saveUserConfig(cfg);
    LOGI("saveAsDefault: %s precision=%s headroom=%.1f unload=%d", ok ? "ok" : "FAILED",
         kPrecisions[prec], head, int(unl));
    sendMessage(OFX::Message::eMessageMessage, "",
                ok ? "Saved as your default for new LTX Alpha Gen nodes."
                   : "Could not write " + userConfigPath());
  }

  void download() {
    std::string token, dir;
    token_->getValue(token);
    modelDir_->getValue(dir);
    LOGI("download pressed: licence=%d token length=%zu dir=%s",
         int(setupState().licenceAccepted), token.size(), dir.c_str());
    if (!setupState().licenceAccepted)
      return error("Accept the LTX-2 Community License before downloading.");
    if (token.empty()) return error("Enter your Hugging Face token first.");
    std::string err;
    const std::string helper = resourcesDir() + "/helper/model_page_helper.py";
    const bool ok = launchDetached({pythonPath(), "-I", "-B", helper, dir},
                                   cleanEnvironment({"HF_TOKEN=" + token}), err);
    token.assign(token.size(), '\0');
    token_->setValue("");  // decision 13: used once, then discarded
    // Decision 41: say what happened to the token instead of leaving a blank field.
    setIfChanged(tokenCaption_, ok ? "Token used " + timeStr("%H:%M") + ", not stored"
                                   : std::string(kTokenCaptionDefault));
    if (!ok) error("Download failed to start: " + err);
    LOGI("download: helper %s %s; token cleared", ok ? "launched" : "FAILED", err.c_str());
  }

  // Bundles logs, environment and job status files for a support request (decision 39).
  void collectLogs() {
    const std::string dir = logDir();
    {
      std::ofstream envf(dir + "/environment.txt");
      for (const auto& line : environmentSnapshot(hostInfo(), resourcesDir())) envf << line << "\n";
    }
    const std::string bundle =
        homeDir() + "/ltx-alphagen-support-" + timeStr("%Y%m%d-%H%M%S") + ".tar.gz";
    std::vector<std::string> argv = {"/usr/bin/tar", "-czf", bundle, "-C",
                                     dir.substr(0, dir.rfind('/')), "logs"};
    const std::string root = cacheRoot();
    struct stat st {};
    if (stat((root + "/jobs").c_str(), &st) == 0) {
      argv.insert(argv.end(), {"-C", root, "jobs"});
    }
    std::string err;
    WorkerProcess tar;
    std::vector<std::string> lines;
    const bool started = tar.start(argv, cleanEnvironment(), err);
    if (started)
      while (tar.readLines(200, lines)) {
      }
    const int status = started ? tar.wait() : -1;
    const bool ok = started && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    for (const auto& l : lines) LOGW("collectLogs: tar: %s", l.c_str());
    LOGI("collectLogs: %s -> %s %s", ok ? "ok" : "FAILED", bundle.c_str(), err.c_str());
    sendMessage(ok ? OFX::Message::eMessageMessage : OFX::Message::eMessageError, "",
                ok ? "Support bundle written to " + bundle +
                         ". Please review it before sending: it contains your logs, system "
                         "details and folder paths, but no images and no token."
                   : "Could not write the support bundle: " + err);
  }

  OFX::Clip* src_;
  OFX::Clip* dst_;
  OFX::ChoiceParam* colour_;
  OFX::StringParam* status_;
  OFX::BooleanParam* showMarker_;
  OFX::ChoiceParam* precision_;
  OFX::BooleanParam* unload_;
  OFX::DoubleParam* headroom_;
  OFX::BooleanParam* accept_;
  OFX::StringParam* tokenCaption_;
  OFX::StringParam* token_;
  OFX::StringParam* modelDir_;
  OFX::StringParam* setupStatus_;
  OFX::BooleanParam* debugLog_;
  OFX::StringParam* cacheDir_;
  OFX::StringParam* cacheInfo_;
  OFX::DoubleParam* simSeconds_;
  OFX::ChoiceParam* simulate_;
  OFX::DoubleParam* simNeed_;
  OFX::StringParam* python_;
  OFX::BooleanParam* enforceGate_;
  OFX::BooleanParam* autoRefresh_;
  OFX::BooleanParam* useDummy_;
  OFX::BooleanParam* initialized_;
  OFX::StringParam* nodeIdParam_;
  OFX::IntParam* refreshTick_;
  std::string nodeId_;
  std::mutex jobMutex_;
  std::string lastJobId_;
  bool inChangedParam_ = false;
  bool loggedOutput_ = false;
  std::thread watcher_;
  std::atomic<bool> stopFlag_{false};
};

template <class D>
D* labelled(D* p, const char* label, const char* hint) {
  p->setLabels(label, label, label);
  if (hint && *hint) p->setHint(hint);
  return p;
}

class AlphaGenFactory : public OFX::PluginFactoryHelper<AlphaGenFactory> {
 public:
  AlphaGenFactory() : PluginFactoryHelper<AlphaGenFactory>(kPluginId, 0, 3) {}

  void describe(OFX::ImageEffectDescriptor& desc) override {
    logConfigure();
    rotateLogs();
    LOGI("==== plugin load (describe) ====");
    for (const auto& line : environmentSnapshot(hostInfo(), resourcesDir()))
      LOGI("env %s", line.c_str());
    const size_t shm = sweepStaleShm();
    const SweepResult sw = sweepCache(cacheSettings(loadUserConfig()));
    LOGI("sweep: %zu stale /dev/shm file(s); cache aged %zu, capped %zu, kept %zu (%.1f MB), "
         "freed %.1f MB",
         shm, sw.aged, sw.capped, sw.kept, sw.bytesKept / 1048576.0, sw.bytesFreed / 1048576.0);
    desc.setLabels(kPluginLabel, kPluginLabel, kPluginLabel);
    desc.setPluginGrouping("LTX");
    desc.addSupportedContext(OFX::eContextFilter);
    desc.addSupportedBitDepth(OFX::eBitDepthFloat);
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);
    desc.setSupportsMultiResolution(false);
    desc.setSupportsTiles(false);
    desc.setTemporalClipAccess(true);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);
    desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);
  }

  void describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum ctx) override {
    try {
      describeParams(desc);
      LOGI("describeInContext ctx=%d done", int(ctx));
    } catch (const std::exception& e) {
      LOGE("describeInContext ctx=%d FAILED: %s [%s]", int(ctx), e.what(), typeid(e).name());
      throw;
    }
  }

  void describeParams(OFX::ImageEffectDescriptor& desc) {
    const Defaults d = computeDefaults();
    LOGI("describeInContext: defaults [%s]", d.source.c_str());

    OFX::ClipDescriptor* src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    src->addSupportedComponent(OFX::ePixelComponentRGBA);
    src->addSupportedComponent(OFX::ePixelComponentRGB);
    src->setTemporalClipAccess(true);
    src->setSupportsTiles(false);
    OFX::ClipDescriptor* dst = desc.defineClip(kOfxImageEffectOutputClipName);
    dst->addSupportedComponent(OFX::ePixelComponentAlpha);
    dst->addSupportedComponent(OFX::ePixelComponentRGBA);
    dst->setSupportsTiles(false);

    // Flame (test 2/3 screenshots): params flow into columns of <= 5 rows, 6 columns per
    // tab; each group starts a column; tabs are named after OFX pages in order. Earlier
    // builds listed every param as a page child and Flame ignored page membership. Here
    // pages list ONLY their groups (allowed by the OFX spec) — Flame still ignored that
    // (2026-10-10). Groups are defined in tab order; tabs come only from overflow, hence the
    // spacer columns below (decision 52).
    OFX::PageParamDescriptor* main = desc.definePageParam("Alpha Gen");
    OFX::PageParamDescriptor* setupPage = desc.definePageParam("Setup");
    OFX::PageParamDescriptor* proto = LTXAG_DEV_BUILD ? desc.definePageParam("Prototype") : nullptr;
    auto group = [&](const char* name, const char* text, OFX::PageParamDescriptor* page) {
      auto* g = desc.defineGroupParam(name);
      g->setLabels(text, text, text);
      g->setOpen(true);
      if (page) page->addChild(*g);
      return g;
    };
    auto place = [](OFX::ParamDescriptor& p, OFX::GroupParamDescriptor* g) { p.setParent(*g); };
    auto label = [&](const char* name, const char* text, const char* def) {
      auto* p = labelled(desc.defineStringParam(name), text, "");
      p->setStringType(OFX::eStringTypeLabel);
      p->setDefault(def);
      p->setIsPersistant(false);
      p->setEvaluateOnChange(false);
      return p;
    };
    auto button = [&](const char* name, const char* text, const char* hint) {
      return labelled(desc.definePushButtonParam(name), text, hint);
    };
    auto perUser = [](OFX::ValueParamDescriptor* p) {
      p->setIsPersistant(false);
      p->setEvaluateOnChange(false);
      p->setAnimates(false);
    };

    // --- Alpha Gen (5 rows) ---
    auto* gMain = group("grpMain", "Alpha Gen", main);
    auto* colour = labelled(desc.defineChoiceParam(kPColour), "Input Colour",
                            "Colour space of the source. It is converted to Rec.1886 through the "
                            "ACES 2.0 SDR 100 nits (Rec.709) view before inference. Bypass: the "
                            "source is already display-referred. (Prototype: not applied yet.)");
    for (const char* c : kColourSpaces) colour->appendOption(c);
    colour->setDefault(0);
    place(*colour, gMain);
    place(*button(kPProcess, "Process",
                  "Generate mattes for the whole clip in the background. Progress and Cancel "
                  "appear in a separate window."),
          gMain);
    place(*label(kPStatus, "Status", "Not processed"), gMain);
    auto* marker = labelled(desc.defineBooleanParam(kPShowMarker), "Show Marker",
                            "Draw a faint 'NOT PROCESSED' marker on frames without a matte. Off: "
                            "such frames are plain black.");
    marker->setDefault(true);
    place(*marker, gMain);

    // --- Performance (5 rows) ---
    auto* gPerf = group("grpPerf", "Performance", main);
    auto* prec = labelled(desc.defineChoiceParam(kPPrecision), "Precision",
                          "bf16: reference quality. fp8: about half the GPU memory.");
    for (const char* p : kPrecisions) prec->appendOption(p);
    prec->setDefault(d.precision);
    place(*prec, gPerf);
    auto* unload = labelled(desc.defineBooleanParam(kPUnload), "Unload GPU",
                            "Return all GPU memory to Flame when this node's processing ends. Turn "
                            "off on all but the last Alpha Gen node to reuse the loaded model.");
    unload->setDefault(d.unload);
    place(*unload, gPerf);
    auto* head = labelled(desc.defineDoubleParam(kPHeadroom), "VRAM Reserve",
                          "GPU memory (GB) kept free for Flame while processing.");
    head->setRange(0.0, 256.0);
    head->setDisplayRange(0.0, 32.0);
    head->setDefault(d.headroomGB);
    place(*head, gPerf);
    place(*button(kPSaveDefault, "Save Default",
                  "Use this column's values for every new LTX Alpha Gen node."),
          gPerf);

    // --- Cache (5 rows) ---
    auto* gCache = group("grpCache", "Cache", main);
    auto* cdir = labelled(desc.defineStringParam(kPCacheDir), "Cache Folder",
                          "Where generated mattes are cached (about 4 MB per HD frame).");
    cdir->setStringType(OFX::eStringTypeDirectoryPath);
    cdir->setDefault(homeDir() + "/.cache/ltx-alphagen/mattes");
    perUser(cdir);
    place(*cdir, gCache);
    place(*label(kPCacheInfo, "Cache Size", ""), gCache);
    place(*button(kPPurgeNode, "Purge Cache", "Delete the cached mattes of this node's clip."),
          gCache);
    place(*button(kPPurgeAll, "Purge All", "Delete every cached matte of every node and project."),
          gCache);

    // --- Spacers (decision 52): Flame ignores page membership and starts a new tab only when
    // the current one passes 6 columns. Three blank columns fill tab 1 (Alpha Gen,
    // Performance, Cache + 3), so Licence/Models overflow to tab 2, which Flame names after
    // the next page ("Setup"). Each spacer holds one blank label so Flame cannot drop it.
    for (int i = 0; i < 3; ++i) {
      const std::string g = "grpSpacer" + std::to_string(i);
      const std::string l = "spacer" + std::to_string(i);
      place(*label(l.c_str(), " ", ""), group(g.c_str(), " ", main));
    }

    // --- Licence (5 rows, per user) ---
    auto* gLic = group("grpLicence", "Licence", setupPage);
    place(*button(kPViewLicence, "View Licence", "Show the LTX-2.x Community License terms."),
          gLic);
    auto* accept = labelled(desc.defineBooleanParam(kPAccept), "I Accept",
                            "I accept the LTX-2.x Community License. Free below USD 10M annual "
                            "revenue; above that a paid Lightricks licence is required.");
    accept->setDefault(false);
    perUser(accept);
    place(*accept, gLic);
    place(*label(kPTokenCaption, "Token", kTokenCaptionDefault), gLic);
    auto* token = labelled(desc.defineStringParam(kPToken), "HF Token",
                           "Your Hugging Face access token (hf_...). Used for this download only, "
                           "never stored.");
    token->setDefault("");
    perUser(token);
    place(*token, gLic);
    place(*button(kPDownload, "Download",
                  "Download the models into the Model Folder (prototype: simulated)."),
          gLic);

    // --- Models (5 rows, per user) ---
    auto* gModels = group("grpModels", "Models", setupPage);
    auto* dir = labelled(desc.defineStringParam(kPModelDir), "Model Folder",
                         "Where the model files are stored (about 71 GB).");
    dir->setStringType(OFX::eStringTypeDirectoryPath);
    dir->setDefault(homeDir() + "/ltx-alphagen-models");
    perUser(dir);
    place(*dir, gModels);
    place(*label(kPSetupStatus, "Setup", ""), gModels);
    auto* dbg = labelled(desc.defineBooleanParam(kPDebugLog), "Debug Logging",
                         "Verbose logging for troubleshooting (per user).");
    dbg->setDefault(false);
    perUser(dbg);
    place(*dbg, gModels);
    place(*button(kPCollectLogs, "Collect Logs",
                  "Write a support bundle (logs and system details) to your home folder."),
          gModels);

    // --- Prototype (dev builds; hidden otherwise) ---
    OFX::GroupParamDescriptor* gProto = proto ? group("grpProto", "Prototype", proto) : nullptr;
    auto* secs = labelled(desc.defineDoubleParam(kPSimSeconds), "Sim Seconds",
                          "Simulated processing time per chunk.");
    secs->setRange(0.0, 3600.0);
    secs->setDisplayRange(0.0, 120.0);
    secs->setDefault(20.0);
    auto* sim = labelled(desc.defineChoiceParam(kPSimulate), "Simulate",
                         "Make the next Process run fail on purpose.");
    for (const char* s : kSimulations) sim->appendOption(s);
    sim->setDefault(0);
    auto* needp = labelled(desc.defineDoubleParam(kPSimNeed), "Sim VRAM GB",
                           "Pretend the run needs this much GPU memory (0 = skip the check).");
    needp->setRange(0.0, 1024.0);
    needp->setDisplayRange(0.0, 128.0);
    needp->setDefault(0.0);
    auto* py = labelled(desc.defineStringParam(kPPython), "Worker Python",
                        "Python used for the worker and helpers.");
    py->setStringType(OFX::eStringTypeFilePath);
    py->setDefault("/usr/bin/python3");
    auto* gate = labelled(desc.defineBooleanParam(kPEnforceGate), "Enforce Gate",
                          "Apply the licence/model gate as a release build would.");
    gate->setDefault(false);
    auto* refresh = labelled(desc.defineBooleanParam(kPAutoRefresh), "Auto Refresh",
                             "EXPERIMENT: refresh the viewer when processing finishes (sets a "
                             "param from a background thread).");
    refresh->setDefault(true);
    auto* dummy = labelled(desc.defineBooleanParam(kPUseDummy), "Use Dummy",
                           "Use the dummy worker (luminance, no GPU) instead of the real model. "
                           "Sim Seconds and Simulate only apply to the dummy.");
    dummy->setDefault(false);
    for (OFX::ValueParamDescriptor* p :
         std::vector<OFX::ValueParamDescriptor*>{secs, sim, needp, py, gate, refresh, dummy}) {
      p->setEvaluateOnChange(false);
      if (gProto)
        place(*p, gProto);
      else
        p->setIsSecret(true);
    }

    // --- Hidden ---
    auto* init = desc.defineBooleanParam(kPInitialized);
    init->setDefault(false);
    init->setIsSecret(true);
    init->setEvaluateOnChange(false);
    auto* nid = desc.defineStringParam(kPNodeId);
    nid->setDefault("");
    nid->setIsSecret(true);
    nid->setEvaluateOnChange(false);
    auto* tick = desc.defineIntParam(kPRefreshTick);
    tick->setDefault(0);
    tick->setIsSecret(true);
    tick->setIsPersistant(false);
    tick->setCanUndo(false);
    tick->setEvaluateOnChange(true);  // the point: a change makes Flame re-render
  }

  OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) override {
    return new AlphaGenEffect(handle);
  }
};

}  // namespace

namespace OFX {
namespace Plugin {
void getPluginIDs(OFX::PluginFactoryArray& ids) {
  static AlphaGenFactory factory;
  ids.push_back(&factory);
}
}  // namespace Plugin
}  // namespace OFX
