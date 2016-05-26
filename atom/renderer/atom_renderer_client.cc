// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/renderer/atom_renderer_client.h"

#include <string>
#include <vector>

#include "atom/browser/web_contents_preferences.h"
#include "atom/common/api/api_messages.h"
#include "atom/common/api/atom_bindings.h"
#include "atom/common/api/event_emitter_caller.h"
#include "atom/common/color_util.h"
#include "atom/common/asar/asar_util.h"
#include "atom/common/javascript_bindings.h"
#include "atom/common/native_mate_converters/string16_converter.h"
#include "atom/common/native_mate_converters/value_converter.h"
#include "atom/common/node_bindings.h"
#include "atom/common/node_includes.h"
#include "atom/common/options_switches.h"
#include "atom/renderer/atom_render_view_observer.h"
#include "atom/renderer/guest_view_container.h"
#include "atom/renderer/node_array_buffer_bridge.h"
#include "base/command_line.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/renderer/media/chrome_key_systems.h"
#include "chrome/renderer/pepper/pepper_helper.h"
#include "chrome/renderer/printing/print_web_view_helper.h"
#include "chrome/renderer/tts_dispatcher.h"
#include "content/public/common/content_constants.h"
#include "content/public/common/isolated_world_ids.h"
#include "content/public/common/url_constants.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/public/renderer/render_thread.h"
#include "content/public/renderer/render_view.h"
#include "ipc/ipc_message_macros.h"
#include "net/base/net_errors.h"
#include "third_party/WebKit/public/web/WebCustomElement.h"
#include "third_party/WebKit/public/web/WebFrameWidget.h"
#include "native_mate/dictionary.h"
#include "net/base/filename_util.h"
#include "third_party/WebKit/public/web/WebDocument.h"
#include "third_party/WebKit/public/web/WebLocalFrame.h"
#include "third_party/WebKit/public/web/WebPluginParams.h"
#include "third_party/WebKit/public/web/WebKit.h"
#include "third_party/WebKit/public/web/WebScriptSource.h"
#include "third_party/WebKit/public/web/WebSecurityPolicy.h"
#include "third_party/WebKit/public/web/WebRuntimeFeatures.h"
#include "third_party/WebKit/public/web/WebView.h"

#if defined(OS_MACOSX)
#include "base/mac/mac_util.h"
#include "base/strings/sys_string_conversions.h"
#endif

#if defined(ENABLE_EXTENSIONS)
#include "atom/renderer/extensions/atom_extensions_renderer_client.h"
#include "atom/common/extensions/atom_extensions_client.h"
#include "extensions/renderer/dispatcher.h"
#endif

#if defined(OS_WIN)
#include <shlobj.h>
#endif

namespace atom {

namespace {

// Helper class to forward the messages to the client.
class AtomRenderFrameObserver : public content::RenderFrameObserver {
 public:
  AtomRenderFrameObserver(content::RenderFrame* frame,
                          AtomRendererClient* renderer_client)
      : content::RenderFrameObserver(frame),
        world_id_(-1),
        renderer_client_(renderer_client) {}

  // content::RenderFrameObserver:
  void DidCreateScriptContext(v8::Handle<v8::Context> context,
                              int extension_group,
                              int world_id) override {
    if (world_id_ != -1 && world_id_ != world_id)
      return;
    world_id_ = world_id;
    renderer_client_->DidCreateScriptContext(context);
  }
  void WillReleaseScriptContext(v8::Local<v8::Context> context,
                                int world_id) override {
    if (world_id_ != world_id)
      return;
    renderer_client_->WillReleaseScriptContext(context);
  }

 private:
  int world_id_;
  AtomRendererClient* renderer_client_;

  DISALLOW_COPY_AND_ASSIGN(AtomRenderFrameObserver);
};

}  // namespace

AtomRendererClient::AtomRendererClient()
    : node_bindings_(NodeBindings::Create(false)),
      atom_bindings_(new AtomBindings) {
#if defined(ENABLE_EXTENSIONS)
  extensions::ExtensionsClient::Set(
      extensions::AtomExtensionsClient::GetInstance());
  extensions::ExtensionsRendererClient::Set(
      extensions::AtomExtensionsRendererClient::GetInstance());
#endif
}

AtomRendererClient::~AtomRendererClient() {
}

void AtomRendererClient::RenderThreadStarted() {
  blink::WebCustomElement::addEmbedderCustomElementName("webview");
  blink::WebCustomElement::addEmbedderCustomElementName("browserplugin");
  OverrideNodeArrayBuffer();

#if defined(ENABLE_EXTENSIONS)
  extensions::AtomExtensionsRendererClient::GetInstance()->
      RenderThreadStarted();
#endif

#if defined(OS_WIN)
  // Set ApplicationUserModelID in renderer process.
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  base::string16 app_id =
      command_line->GetSwitchValueNative(switches::kAppUserModelId);
  if (!app_id.empty()) {
    SetCurrentProcessExplicitAppUserModelID(app_id.c_str());
  }
#endif

#if defined(OS_MACOSX)
  // Disable rubber banding by default.
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(switches::kScrollBounce)) {
    base::ScopedCFTypeRef<CFStringRef> key(
        base::SysUTF8ToCFStringRef("NSScrollViewRubberbanding"));
    base::ScopedCFTypeRef<CFStringRef> value(
        base::SysUTF8ToCFStringRef("false"));
    CFPreferencesSetAppValue(key, value, kCFPreferencesCurrentApplication);
    CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
  }
#endif
}

void AtomRendererClient::RenderFrameCreated(
    content::RenderFrame* render_frame) {
#if defined(ENABLE_EXTENSIONS)
  extensions::AtomExtensionsRendererClient::GetInstance()->RenderFrameCreated(
    render_frame);
#endif

  new PepperHelper(render_frame);

  // Allow file scheme to handle service worker by default.
  blink::WebSecurityPolicy::registerURLSchemeAsAllowingServiceWorkers("file");

  if (!render_frame->IsMainFrame())
    return;

  if (WebContentsPreferences::run_node())
    new AtomRenderFrameObserver(render_frame, this);
}

void AtomRendererClient::RenderViewCreated(content::RenderView* render_view) {
  new printing::PrintWebViewHelper(render_view);
  new AtomRenderViewObserver(render_view, this);

  blink::WebFrameWidget* web_frame_widget = render_view->GetWebFrameWidget();
  if (!web_frame_widget)
    return;

  base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  if (cmd->HasSwitch(switches::kGuestInstanceID)) {  // webview.
    web_frame_widget->setBaseBackgroundColor(SK_ColorTRANSPARENT);
  } else {  // normal window.
    // If backgroundColor is specified then use it.
    std::string name = cmd->GetSwitchValueASCII(switches::kBackgroundColor);
    // Otherwise use white background.
    SkColor color = name.empty() ? SK_ColorWHITE : ParseHexColor(name);
    web_frame_widget->setBaseBackgroundColor(color);
  }

#if defined(ENABLE_EXTENSIONS)
  extensions::AtomExtensionsRendererClient::GetInstance()->
      RenderViewCreated(render_view);
#endif
}

void AtomRendererClient::RunScriptsAtDocumentStart(
    content::RenderFrame* render_frame) {
  // Make sure every page will get a script context created.
  render_frame->GetWebFrame()->executeScript(
      blink::WebScriptSource("void 0"));
#if defined(ENABLE_EXTENSIONS)
  extensions::AtomExtensionsRendererClient::GetInstance()->
      RunScriptsAtDocumentStart(render_frame);
  // |render_frame| might be dead by now.
#endif
}

blink::WebSpeechSynthesizer* AtomRendererClient::OverrideSpeechSynthesizer(
    blink::WebSpeechSynthesizerClient* client) {
  return new TtsDispatcher(client);
}

bool AtomRendererClient::OverrideCreatePlugin(
    content::RenderFrame* render_frame,
    blink::WebLocalFrame* frame,
    const blink::WebPluginParams& params,
    blink::WebPlugin** plugin) {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (params.mimeType.utf8() == content::kBrowserPluginMimeType ||
      command_line->HasSwitch(switches::kEnablePlugins))
    return false;

  *plugin = nullptr;
  return true;
}

void AtomRendererClient::DidCreateScriptContext(
    v8::Handle<v8::Context> context) {
  // Whether the node binding has been initialized.
  bool first_time = node_bindings_->uv_env() == nullptr;

  // Prepare the node bindings.
  if (first_time) {
    node_bindings_->Initialize();
    node_bindings_->PrepareMessageLoop();
  }

  // Setup node environment for each window.
  node::Environment* env = node_bindings_->CreateEnvironment(context);

  // Add atom-shell extended APIs.
  atom_bindings_->BindTo(env->isolate(), env->process_object());

  // Load everything.
  node_bindings_->LoadEnvironment(env);

  if (first_time) {
    // Make uv loop being wrapped by window context.
    node_bindings_->set_uv_env(env);

    // Give the node loop a run to make sure everything is ready.
    node_bindings_->RunMessageLoop();
  }
}

void AtomRendererClient::WillReleaseScriptContext(
    v8::Handle<v8::Context> context) {
  if (WebContentsPreferences::run_node()) {
    node::Environment* env = node::Environment::GetCurrent(context);
    if (env)
      mate::EmitEvent(env->isolate(), env->process_object(), "exit");
  }
}

bool AtomRendererClient::AllowPopup() {
  if (WebContentsPreferences::run_node()) {
    return false;  // TODO(bridiver) - should return setting for allow popups
  }

#if defined(ENABLE_EXTENSIONS)
  return extensions::AtomExtensionsRendererClient::GetInstance()->AllowPopup();
#else
  return false;
#endif
}

bool AtomRendererClient::ShouldFork(blink::WebLocalFrame* frame,
                                    const GURL& url,
                                    const std::string& http_method,
                                    bool is_initial_navigation,
                                    bool is_server_redirect,
                                    bool* send_referrer) {
  if (WebContentsPreferences::run_node()) {
    *send_referrer = true;
    return http_method == "GET" && !is_server_redirect;
  }

  return false;
}

content::BrowserPluginDelegate* AtomRendererClient::CreateBrowserPluginDelegate(
    content::RenderFrame* render_frame,
    const std::string& mime_type,
    const GURL& original_url) {
  if (mime_type == content::kBrowserPluginMimeType) {
    return new GuestViewContainer(render_frame);
  } else {
    return nullptr;
  }
}

void AtomRendererClient::AddKeySystems(
    std::vector<media::KeySystemInfo>* key_systems) {
  AddChromeKeySystems(key_systems);
}

void AtomRendererClient::GetNavigationErrorStrings(
    content::RenderFrame* render_frame,
    const blink::WebURLRequest& failed_request,
    const blink::WebURLError& error,
    std::string* error_html,
    base::string16* error_description) {
  if (!error_description)
    return;

  *error_description = base::UTF8ToUTF16(net::ErrorToShortString(error.reason));
}

bool AtomRendererClient::WillSendRequest(
    blink::WebFrame* frame,
    ui::PageTransition transition_type,
    const GURL& url,
    const GURL& first_party_for_cookies,
    GURL* new_url) {
  // Check whether the request should be allowed. If not allowed, we reset the
  // URL to something invalid to prevent the request and cause an error.
#if defined(ENABLE_EXTENSIONS)
  if (extensions::AtomExtensionsRendererClient::GetInstance()->WillSendRequest(
          frame, transition_type, url, new_url))
    return true;
#endif

  return false;
}

void AtomRendererClient::RunScriptsAtDocumentEnd(
    content::RenderFrame* render_frame) {
#if defined(ENABLE_EXTENSIONS)
  extensions::AtomExtensionsRendererClient::GetInstance()->
      RunScriptsAtDocumentEnd(render_frame);
  // |render_frame| might be dead by now.
#endif
}

void AtomRendererClient::DidInitializeServiceWorkerContextOnWorkerThread(
    v8::Local<v8::Context> context,
    const GURL& url) {
#if defined(ENABLE_EXTENSIONS)
  extensions::Dispatcher::DidInitializeServiceWorkerContextOnWorkerThread(
      context, url);
#endif
}

void AtomRendererClient::WillDestroyServiceWorkerContextOnWorkerThread(
    v8::Local<v8::Context> context,
    const GURL& url) {
#if defined(ENABLE_EXTENSIONS)
  extensions::Dispatcher::WillDestroyServiceWorkerContextOnWorkerThread(context,
                                                                        url);
#endif
}

}  // namespace atom
