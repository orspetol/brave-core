/* Copyright (c) 2020 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 3.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/browser/brave_cosmetic_resources_tab_helper.h"

#include <memory>
#include <string>
#include <vector>
#include <utility>

#include "base/json/json_writer.h"
#include "brave/browser/brave_browser_process_impl.h"
#include "brave/components/brave_shields/browser/ad_block_custom_filters_service.h"
#include "brave/components/brave_shields/browser/ad_block_regional_service_manager.h"
#include "brave/components/brave_shields/browser/ad_block_service.h"
#include "brave/components/brave_shields/browser/ad_block_service_helper.h"
#include "brave/components/brave_shields/browser/brave_shields_util.h"
#include "brave/content/browser/cosmetic_filters_communication_impl.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_isolated_world_ids.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/renderer/render_frame.h"
#include "brave/components/cosmetic_filters/resources/grit/cosmetic_filters_generated_map.h"
#include "ui/base/resource/resource_bundle.h"

std::vector<std::string>
    BraveCosmeticResourcesTabHelper::vetted_search_engines_ = {
  "duckduckgo",
  "qwant",
  "bing",
  "startpage",
  "yahoo",
  "onesearch",
  "google",
  "yandex"
};

// Doing that to prevent a lint error:
// Static/global string variables are not permitted
// You can create an object dynamically and never delete it by using a
// Function-local static pointer or reference
// (e.g., static const auto& impl = *new T(args...);).
std::string* BraveCosmeticResourcesTabHelper::observing_script_ =
    new std::string("");


namespace {

bool ShouldDoCosmeticFiltering(content::WebContents* contents,
    const GURL& url) {
  Profile* profile = Profile::FromBrowserContext(contents->GetBrowserContext());
  auto* map = HostContentSettingsMapFactory::GetForProfile(profile);

  return ::brave_shields::ShouldDoCosmeticFiltering(map, url);
}

std::string LoadDataResource(const int& id) {
  std::string data_resource;

  auto& resource_bundle = ui::ResourceBundle::GetSharedInstance();
  if (resource_bundle.IsGzipped(id)) {
    data_resource = resource_bundle.LoadDataResourceString(id);
  } else {
    data_resource = resource_bundle.GetRawDataResource(id).as_string();
  }

  return data_resource;
}

std::unique_ptr<base::ListValue> GetUrlCosmeticResourcesOnTaskRunner(
    const std::string& url) {
  auto result_list = std::make_unique<base::ListValue>();

  base::Optional<base::Value> resources = g_brave_browser_process->
      ad_block_service()->UrlCosmeticResources(url);

  if (!resources || !resources->is_dict()) {
    return result_list;
  }

  base::Optional<base::Value> regional_resources = g_brave_browser_process->
      ad_block_regional_service_manager()->UrlCosmeticResources(url);

  if (regional_resources && regional_resources->is_dict()) {
    ::brave_shields::MergeResourcesInto(
        std::move(*regional_resources), &*resources, /*force_hide=*/false);
  }

  base::Optional<base::Value> custom_resources = g_brave_browser_process->
      ad_block_custom_filters_service()->UrlCosmeticResources(url);

  if (custom_resources && custom_resources->is_dict()) {
    ::brave_shields::MergeResourcesInto(
        std::move(*custom_resources), &*resources, /*force_hide=*/true);
  }

  result_list->Append(std::move(*resources));

  return result_list;
}

std::unique_ptr<base::ListValue> GetHiddenClassIdSelectorsOnTaskRunner(
    const std::vector<std::string>& classes,
    const std::vector<std::string>& ids,
    const std::vector<std::string>& exceptions) {
  base::Optional<base::Value> hide_selectors = g_brave_browser_process->
      ad_block_service()->HiddenClassIdSelectors(classes, ids, exceptions);

  base::Optional<base::Value> regional_selectors = g_brave_browser_process->
      ad_block_regional_service_manager()->
          HiddenClassIdSelectors(classes, ids, exceptions);

  base::Optional<base::Value> custom_selectors = g_brave_browser_process->
      ad_block_custom_filters_service()->
          HiddenClassIdSelectors(classes, ids, exceptions);

  if (hide_selectors && hide_selectors->is_list()) {
    if (regional_selectors && regional_selectors->is_list()) {
      for (auto i = regional_selectors->GetList().begin();
          i < regional_selectors->GetList().end(); i++) {
        hide_selectors->Append(std::move(*i));
      }
    }
  } else {
    hide_selectors = std::move(regional_selectors);
  }

  auto result_list = std::make_unique<base::ListValue>();
  if (hide_selectors && hide_selectors->is_list()) {
    result_list->Append(std::move(*hide_selectors));
  }
  if (custom_selectors && custom_selectors->is_list()) {
    result_list->Append(std::move(*custom_selectors));
  }

  return result_list;
}

bool IsVettedSearchEngine(const std::string& host) {
  for (size_t i = 0;
      i < BraveCosmeticResourcesTabHelper::vetted_search_engines_.size();
      i++) {
    size_t found_pos = host.find(
        BraveCosmeticResourcesTabHelper::vetted_search_engines_[i]);
    if (found_pos != std::string::npos) {
      size_t last_dot_pos = host.find(".", found_pos + 1);
      if (last_dot_pos == std::string::npos) {
        return false;
      }
      if (host.find(".", last_dot_pos + 1) == std::string::npos) {
        return true;
      }
    }
  }

  return false;
}

}  // namespace


BraveCosmeticResourcesTabHelper::BraveCosmeticResourcesTabHelper(
    content::WebContents* contents)
    : WebContentsObserver(contents),
    enabled_1st_party_cf_filtering_(false) {
  if (BraveCosmeticResourcesTabHelper::observing_script_->empty()) {
    *BraveCosmeticResourcesTabHelper::observing_script_ =
        LoadDataResource(kCosmeticFiltersGenerated[0].value);
  }
}

BraveCosmeticResourcesTabHelper::~BraveCosmeticResourcesTabHelper() {
}

void BraveCosmeticResourcesTabHelper::GetUrlCosmeticResourcesOnUI(
    content::GlobalFrameRoutingId frame_id, const std::string& url,
    bool do_non_scriplets, std::unique_ptr<base::ListValue> resources) {
  if (!resources) {
    return;
  }
  for (auto i = resources->GetList().begin();
      i < resources->GetList().end(); i++) {
    base::DictionaryValue* resources_dict;
    if (!i->GetAsDictionary(&resources_dict)) {
      continue;
    }
    std::string to_inject;
    resources_dict->GetString("injected_script", &to_inject);
    auto* frame_host = content::RenderFrameHost::FromID(frame_id);
    if (!frame_host)
      return;
    if (do_non_scriplets) {
      Profile* profile = Profile::FromBrowserContext(
          web_contents()->GetBrowserContext());
      enabled_1st_party_cf_filtering_ =
          brave_shields::IsFirstPartyCosmeticFilteringEnabled(
              HostContentSettingsMapFactory::GetForProfile(profile),
                  GURL(url));
      bool generichide = false;
      resources_dict->GetBoolean("generichide", &generichide);
      std::string pre_init_script =
        "(function() {"
        "if (window.content_cosmetic == undefined) {"
          "window.content_cosmetic = new Object();}"
        "if (window.content_cosmetic.hide1pContent === undefined) {"
          "window.content_cosmetic.hide1pContent = ";
      if (enabled_1st_party_cf_filtering_) {
        pre_init_script += "true";
      } else {
        pre_init_script += "false";
      }
      pre_init_script += ";"
        "}"
        "if (window.content_cosmetic.generichide === undefined) {"
          "window.content_cosmetic.generichide = ";
      if (generichide) {
        pre_init_script += "true";
      } else {
        pre_init_script += "false";
      }
      pre_init_script += ";"
        "}"
        "})();";
      frame_host->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(pre_init_script),
        base::NullCallback(), ISOLATED_WORLD_ID_CHROME_INTERNAL);
      frame_host->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(*BraveCosmeticResourcesTabHelper::observing_script_),
        base::NullCallback(), ISOLATED_WORLD_ID_CHROME_INTERNAL);
    }
    if (to_inject.length() > 1) {
      std::string script =
        "(function() {"
          "let script;"
          "let text = " + to_inject + ";"
          "try {"
            "script = document.createElement('script');"
            "const textnode = document.createTextNode(text);"
            "script.appendChild(textnode);"
            "(document.head || document.documentElement).appendChild(script);"
          "} catch (ex) {"
          "}"
          "if (script) {"
            "if (script.parentNode) {"
              "script.parentNode.removeChild(script);"
            "}"
            "script.textContent = '';"
          "}"
        "})();";
      frame_host->ExecuteJavaScriptInIsolatedWorld(
          base::UTF8ToUTF16(script),
          base::NullCallback(), ISOLATED_WORLD_ID_CHROME_INTERNAL);
    }
    // Working on css rules, we do that on a main frame only
    if (!do_non_scriplets)
      return;
    CSSRulesRoutine(url, resources_dict, frame_id);
  }
}

void BraveCosmeticResourcesTabHelper::CSSRulesRoutine(
    const std::string& url_string, base::DictionaryValue* resources_dict,
    content::GlobalFrameRoutingId frame_id) {
  // Check are first party cosmetic filters enabled
  const GURL url(url_string);
  if (url.is_empty() || !url.is_valid() || IsVettedSearchEngine(url.host())) {
    return;
  }

  base::ListValue* cf_exceptions_list;
  if (resources_dict->GetList("exceptions", &cf_exceptions_list)) {
    for (size_t i = 0; i < cf_exceptions_list->GetSize(); i++) {
      exceptions_.push_back(cf_exceptions_list->GetList()[i].GetString());
    }
  }
  base::ListValue* hide_selectors_list;
  base::ListValue* force_hide_selectors_list = nullptr;
  if (resources_dict->GetList("hide_selectors", &hide_selectors_list)) {
    if (enabled_1st_party_cf_filtering_) {
      force_hide_selectors_list = hide_selectors_list;
    } else {
      std::string cosmeticFilterConsiderNewSelectors_script =
          "(function() {"
            "let nextIndex ="
              "window.content_cosmetic.cosmeticStyleSheet.rules.length;";
      std::string json_selectors;
      base::JSONWriter::Write(*hide_selectors_list, &json_selectors);
      if (!json_selectors.empty()) {
        cosmeticFilterConsiderNewSelectors_script += "const selectors = " +
            json_selectors;
      } else {
        cosmeticFilterConsiderNewSelectors_script += "const selectors = []";
      }
      cosmeticFilterConsiderNewSelectors_script += ";"
          "selectors.forEach(selector => {"
            "if (!window.content_cosmetic.allSelectorsToRules.has(selector)) {"
              "let rule = selector + '{display:none !important;}';"
              "window.content_cosmetic.cosmeticStyleSheet.insertRule("
                "`${rule}`, nextIndex);"
              "window.content_cosmetic.allSelectorsToRules.set("
                "selector, nextIndex);"
              "nextIndex++;"
              "window.content_cosmetic.firstRunQueue.add(selector);"
            "}"
          "});"
          "if (!document.adoptedStyleSheets.includes("
                "window.content_cosmetic.cosmeticStyleSheet)){"
              "document.adoptedStyleSheets ="
                "[window.content_cosmetic.cosmeticStyleSheet];"
          "};"
          "})();";
      if (hide_selectors_list->GetSize() != 0) {
        auto* frame_host = content::RenderFrameHost::FromID(frame_id);
        if (!frame_host)
          return;
        frame_host->ExecuteJavaScriptInIsolatedWorld(
            base::UTF8ToUTF16(cosmeticFilterConsiderNewSelectors_script),
            base::NullCallback(), ISOLATED_WORLD_ID_CHROME_INTERNAL);
      }
    }
  }

  std::string styled_stylesheet = "";
  if (force_hide_selectors_list!= nullptr &&
      force_hide_selectors_list->GetSize() != 0) {
    for (size_t i = 0; i < force_hide_selectors_list->GetSize(); i++) {
      if (i != 0) {
        styled_stylesheet += ",";
      }
      styled_stylesheet += force_hide_selectors_list->GetList()[i].GetString();
    }
    styled_stylesheet += "{display:none!important;}\n";
  }
  base::DictionaryValue* style_selectors_dictionary = nullptr;
  if (resources_dict->GetDictionary("style_selectors",
      &style_selectors_dictionary)) {
    for (const auto& it : style_selectors_dictionary->DictItems()) {
      for (size_t i = 0; i < it.second.GetList().size(); i++) {
        if (i != 0) {
          styled_stylesheet += ";";
        } else {
          styled_stylesheet += it.first + "{";
        }
        styled_stylesheet += it.second.GetList()[i].GetString();
      }
      if (it.second.GetList().size() != 0) {
        styled_stylesheet += ";}\n";
      }
    }
    if (!styled_stylesheet.empty()) {
      std::string cosmeticFilterConsiderNewSelectors_script =
          "(function() {"
            "let nextIndex ="
              "window.content_cosmetic.cosmeticStyleSheet.rules.length;";
      cosmeticFilterConsiderNewSelectors_script +=
            "window.content_cosmetic.cosmeticStyleSheet.insertRule(`" +
              styled_stylesheet + "`, nextIndex);"
            "window.content_cosmetic.allSelectorsToRules.set(`" +
                styled_stylesheet + "`, nextIndex);"
            "nextIndex++;";
      cosmeticFilterConsiderNewSelectors_script +=
            "if (!document.adoptedStyleSheets.includes("
                  "window.content_cosmetic.cosmeticStyleSheet)){"
               "document.adoptedStyleSheets ="
                 "[window.content_cosmetic.cosmeticStyleSheet];"
            "};";
      cosmeticFilterConsiderNewSelectors_script +=
          "})();";
      auto* frame_host = content::RenderFrameHost::FromID(frame_id);
      if (!frame_host)
        return;
      frame_host->ExecuteJavaScriptInIsolatedWorld(
          base::UTF8ToUTF16(cosmeticFilterConsiderNewSelectors_script),
          base::NullCallback(), ISOLATED_WORLD_ID_CHROME_INTERNAL);
    }
  }

  if (!enabled_1st_party_cf_filtering_) {
    auto* frame_host = content::RenderFrameHost::FromID(frame_id);
    if (!frame_host)
      return;
    frame_host->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(
            *BraveCosmeticResourcesTabHelper::observing_script_),
        base::NullCallback(), ISOLATED_WORLD_ID_CHROME_INTERNAL);
  }
}

void BraveCosmeticResourcesTabHelper::GetHiddenClassIdSelectorsOnUI(
    content::GlobalFrameRoutingId frame_id, const GURL& url,
    std::unique_ptr<base::ListValue> selectors) {
  if (!selectors || IsVettedSearchEngine(url.host())) {
    return;
  }
  std::string cosmeticFilterConsiderNewSelectors_script =
      "(function() {"
        "let nextIndex ="
          "window.content_cosmetic.cosmeticStyleSheet.rules.length;";
  bool execute_script = false;
  for (size_t i = 0; i < selectors->GetSize(); i++) {
    base::ListValue* selectors_list = nullptr;
    if (!selectors->GetList()[i].GetAsList(&selectors_list) ||
        selectors_list->GetSize() == 0) {
      continue;
    }
    std::string json_selectors;
    base::JSONWriter::Write(*selectors_list, &json_selectors);
    if (!json_selectors.empty()) {
      cosmeticFilterConsiderNewSelectors_script += "const selectors = " +
          json_selectors;
      execute_script = true;
    } else {
      cosmeticFilterConsiderNewSelectors_script += "const selectors = []";
    }
  }
  if (execute_script) {
    cosmeticFilterConsiderNewSelectors_script += ";"
      "selectors.forEach(selector => {"
        "if (!window.content_cosmetic.allSelectorsToRules.has(selector)) {"
          "let rule = selector + '{display:none !important;}';"
          "window.content_cosmetic.cosmeticStyleSheet.insertRule("
            "`${rule}`, nextIndex);"
          "window.content_cosmetic.allSelectorsToRules.set("
            "selector, nextIndex);"
          "nextIndex++;"
          "window.content_cosmetic.firstRunQueue.add(selector);"
        "}"
      "});"
      "if (!document.adoptedStyleSheets.includes("
          "window.content_cosmetic.cosmeticStyleSheet)){"
        "document.adoptedStyleSheets ="
          "[window.content_cosmetic.cosmeticStyleSheet];"
      "};"
      "})();";
    auto* frame_host = content::RenderFrameHost::FromID(frame_id);
    if (!frame_host)
      return;
    frame_host->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(cosmeticFilterConsiderNewSelectors_script),
        base::NullCallback(), ISOLATED_WORLD_ID_CHROME_INTERNAL);
  }

  if (!enabled_1st_party_cf_filtering_) {
    auto* frame_host = content::RenderFrameHost::FromID(frame_id);
    if (!frame_host)
      return;
    frame_host->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(
            *BraveCosmeticResourcesTabHelper::observing_script_),
        base::NullCallback(), ISOLATED_WORLD_ID_CHROME_INTERNAL);
  }
}

void BraveCosmeticResourcesTabHelper::ProcessURL(
    content::RenderFrameHost* render_frame_host, const GURL& url,
    const bool& do_non_scriplets) {
  content::CosmeticFiltersCommunicationImpl::CreateInstance(render_frame_host,
      this);
  if (!render_frame_host || !ShouldDoCosmeticFiltering(web_contents(), url)) {
    return;
  }
  g_brave_browser_process->ad_block_service()->GetTaskRunner()->
      PostTaskAndReplyWithResult(FROM_HERE,
          base::BindOnce(GetUrlCosmeticResourcesOnTaskRunner,
            url.spec()),
          base::BindOnce(&BraveCosmeticResourcesTabHelper::
            GetUrlCosmeticResourcesOnUI, AsWeakPtr(),
            content::GlobalFrameRoutingId(
                  render_frame_host->GetProcess()->GetID(),
                  render_frame_host->GetRoutingID()),
            url.spec(), do_non_scriplets));
}

void BraveCosmeticResourcesTabHelper::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!navigation_handle)
    return;
  ProcessURL(navigation_handle->GetRenderFrameHost(),
      web_contents()->GetLastCommittedURL(),
      navigation_handle->IsInMainFrame());
}

void BraveCosmeticResourcesTabHelper::ResourceLoadComplete(
    content::RenderFrameHost* render_frame_host,
    const content::GlobalRequestID& request_id,
    const blink::mojom::ResourceLoadInfo& resource_load_info) {
  ProcessURL(render_frame_host, resource_load_info.final_url, false);
}

void BraveCosmeticResourcesTabHelper::HiddenClassIdSelectors(
    content::RenderFrameHost* render_frame_host,
    const std::vector<std::string>& classes,
    const std::vector<std::string>& ids) {
  g_brave_browser_process->ad_block_service()->GetTaskRunner()->
      PostTaskAndReplyWithResult(FROM_HERE,
          base::BindOnce(&GetHiddenClassIdSelectorsOnTaskRunner, classes, ids,
              exceptions_),
          base::BindOnce(&BraveCosmeticResourcesTabHelper::
              GetHiddenClassIdSelectorsOnUI, AsWeakPtr(),
              content::GlobalFrameRoutingId(
                  render_frame_host->GetProcess()->GetID(),
                  render_frame_host->GetRoutingID()),
              web_contents()->GetLastCommittedURL()));
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(BraveCosmeticResourcesTabHelper)