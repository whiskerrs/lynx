// Copyright 2021 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef CORE_RENDERER_DOM_FIBER_LIST_ELEMENT_H_
#define CORE_RENDERER_DOM_FIBER_LIST_ELEMENT_H_

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/renderer/dom/element_manager.h"
#include "core/renderer/dom/fiber/fiber_element.h"
#include "core/renderer/ui_component/list/list_container_delegate_internal.h"
#include "core/renderer/ui_component/list/mediator/list_mediator.h"
#include "core/renderer/ui_wrapper/layout/list_node.h"

namespace lynx {
namespace tasm {

class TemplateAssembler;
class ListElement;

class ListElementSSRHelper {
 public:
  explicit ListElementSSRHelper(ListElement* list) : list_element_(list) {}

  // move only
  ListElementSSRHelper(const ListElementSSRHelper&) = delete;
  ListElementSSRHelper& operator=(const ListElementSSRHelper&) = delete;
  ListElementSSRHelper(ListElementSSRHelper&&) = default;
  ListElementSSRHelper& operator=(ListElementSSRHelper&&) = default;

  void OnEnqueueComponent(int32_t sign);
  void HydrateListNode();
  // on list element get callback function.
  void OnListElementHydrateFinish();
  int32_t ComponentAtIndexInSSR(uint32_t index, int64_t operationId);

  bool HasHydrate() { return has_hydrate_; }
  void AppendChild(fml::RefPtr<FiberElement> child) {
    ssr_elements_.push_back({child, SSRItemStatus::kWaitingRender});
  }

 private:
  enum class SSRItemStatus : uint32_t {
    kWaitingRender = 0,
    kRendered = 1,
    kEnqueued = 2,
  };

  bool has_hydrate_ = false;
  ListElement* list_element_;
  std::vector<std::pair<fml::RefPtr<FiberElement>, SSRItemStatus>>
      ssr_elements_;
};

// Native (non-lepus) item provider for `ListElement`. Mirrors the
// callback contract of `componentAtIndex` / `enqueueComponent` /
// `componentAtIndexes` so a C++ embedder (e.g. a Rust runtime
// without a JS framework) can drive the list directly. The native
// path takes priority over the lepus path in `ListElement` if
// installed via `SetNativeItemProvider`; otherwise the existing
// lepus / SSR paths are used as-is.
struct ListNativeItemProvider {
  // Called when the list needs the element for `index`. Implementation
  // must create or look up a child `FiberElement` and return its
  // `impl_id`, or `list::kInvalidIndex` (= 0) on failure.
  std::function<int32_t(uint32_t index, int64_t operation_id,
                        bool enable_reuse_notification)>
      component_at_index;

  // Called when the element identified by `sign` is leaving the
  // viewport. The provider may pool the element for reuse or release
  // it. Optional — leave empty for no-op recycling.
  std::function<void(int32_t sign)> enqueue_component;

  // Optional batch variant. If empty, `ListElement::ComponentAtIndexes`
  // falls back to looping `component_at_index`. Provided for prefetch
  // / parallel-render optimisations the embedder may wish to drive.
  std::function<void(const std::vector<uint32_t>& indices,
                     const std::vector<int64_t>& operation_ids,
                     bool enable_reuse_notification)>
      component_at_indexes;
};

class ListElement : public FiberElement, public tasm::ListNode {
 public:
  ListElement(ElementManager* manager, const base::String& tag,
              const lepus::Value& component_at_index,
              const lepus::Value& enqueue_component,
              const lepus::Value& component_at_indexes);

  fml::RefPtr<FiberElement> CloneElement(
      bool clone_resolved_props) const override {
    return fml::AdoptRef<FiberElement>(
        new ListElement(*this, clone_resolved_props));
  }
  // Body moved to .cc so the header doesn't have to drag in
  // `jsvalue_helper.h` (and its `quickjs/include/trace-gc.h`), which
  // lets embedders without the QuickJS header set include
  // `list_element.h` cleanly.
  void visitor(void* rt, void* func, uint64_t trace_tool) override;

  ~ListElement() override = default;

  virtual ListNode* GetListNode() override;

  void set_tasm(TemplateAssembler* tasm) { tasm_ = tasm; }

  bool is_list() const override { return true; }

  const StyleMap* PeekCommittedStylesFromAttributes() const override;

  void TickElement(fml::TimePoint& time) override;
  void AppendComponentInfo(std::unique_ptr<ListComponentInfo> info) override {}
  void RemoveComponent(uint32_t sign) override {}
  void RenderComponentAtIndex(uint32_t row, int64_t operationId = 0) override {}
  void UpdateComponent(uint32_t sign, uint32_t row,
                       int64_t operationId = 0) override {}

  int32_t ComponentAtIndex(uint32_t index, int64_t operationId,
                           bool enable_reuse_notification) override;

  void ComponentAtIndexes(const fml::RefPtr<lepus::CArray>& index_array,
                          const fml::RefPtr<lepus::CArray>& operation_id_array,
                          bool enable_reuse_notification = false) override;

  void EnqueueComponent(int32_t sign) override;

  void UpdateCallbacks(const lepus::Value& component_at_index,
                       const lepus::Value& enqueue_component,
                       const lepus::Value& component_at_indexes);
  static bool IsTemplateCallbackAttribute(const base::String& key);
  bool ApplyTemplateCallbackAttribute(const base::String& key,
                                      const lepus::Value& value);

  void NotifyListReuseNode(const fml::RefPtr<FiberElement>& child,
                           const base::String& item_key);

  void OnListItemBatchFinished(
      const std::shared_ptr<PipelineOptions>& options) override;

  // When the list element changes, this method will be invoked. For example, if
  // the list's width or height changes, or if the List itself has new diff
  // information.
  void OnListElementUpdated(
      const std::shared_ptr<PipelineOptions>& options) override;
  // When the rendering of the list's child node is complete, this method will
  // be invoked. In this method, we can obtain the correct layout information
  // of the child node.
  void OnComponentFinished(
      Element* component,
      const std::shared_ptr<PipelineOptions>& option) override;
  // Receive drag distance from platform list container.
  void ScrollByListContainer(float content_offset_x, float content_offset_y,
                             float original_x, float original_y) override;
  void ScrollToPosition(int index, float offset, int align,
                        bool smooth) override;
  void OnListItemLayoutUpdated(Element* component) override;
  void ScrollStopped() override;
  bool DisableListPlatformImplementation() const override {
    return disable_list_platform_implementation_
               ? *disable_list_platform_implementation_
               : false;
  }
  void SetEventHandler(const base::String& name,
                       EventHandler* handler) override;

  void ResetEventHandlers() override;

  ParallelFlushReturn PrepareForCreateOrUpdate() override;

  bool ResolveStyleValue(CSSPropertyID id, const CSSValue& value) override;

  void PropsUpdateFinish() override;

  virtual void ParallelFlushAsRoot() override;

  void SetSsrHelper(ListElementSSRHelper ssr_helper) {
    ssr_helper_ = std::move(ssr_helper);
  }

  // Install a native (non-lepus) item provider. While installed, the
  // list's `ComponentAtIndex` / `EnqueueComponent` /
  // `ComponentAtIndexes` route to the provider's callbacks ahead of
  // the lepus path. Intended for embedders without a JS runtime
  // (e.g. Whisker). The SSR path still takes priority during
  // hydration so existing SSR behaviour is unchanged.
  void SetNativeItemProvider(ListNativeItemProvider provider) {
    native_item_provider_ = std::move(provider);
  }

  // True iff a native item provider is installed and supplies the
  // mandatory `component_at_index` callback.
  bool HasNativeItemProvider() const {
    return static_cast<bool>(native_item_provider_.component_at_index);
  }

  // Tear down the native provider (releases captured state, e.g.
  // an embedder's `Box<dyn Fn>` held inside the std::function).
  void ClearNativeItemProvider() { native_item_provider_ = {}; }

  void set_will_destroy(bool destroy) override;

  // ssr hydrate.
  void Hydrate();
  void HydrateFinish();

  void SetupFragmentBehavior(Fragment* fragment) override;

  virtual const base::String& GetPlatformNodeTag() const override {
    return platform_node_tag_;
  };

  void AttachToElementManager(
      ElementManager* manager,
      const std::shared_ptr<CSSStyleSheetManager>& style_manager,
      bool keep_element_id) override;

  void UpdateLayoutNodeAttribute(starlight::LayoutAttribute key,
                                 const lepus::Value& value) override;

  void FlushListContainerInfo(const base::String& key,
                              const lepus::Value& value);

 protected:
  // Currently, the list element does not copy any member variables and is an
  // empty implementation.
  // TODO(WUJINTIAN): copy fiber list element
  ListElement(const ListElement& element, bool clone_resolved_props)
      : FiberElement(element, clone_resolved_props) {}

  void OnNodeAdded(FiberElement* child) override;
  void FilterComponents(
      std::vector<std::unique_ptr<ListComponentInfo>>& components,
      tasm::TemplateAssembler* tasm) override {}
  bool HasComponent(const std::string& component_name,
                    const std::string& current_entry) override {
    return false;
  }
  void SetAttributeInternal(const base::String& key,
                            const lepus::Value& value) override;
  void ResetAttribute(const base::String& key) override;
  void CacheCommittedStyleFromAttributes(CSSPropertyID id,
                                         const CSSValue& value) override;
  void CacheCommittedStyleFromAttributes(CSSPropertyID id,
                                         const lepus::Value& value) override;
  void RemoveCommittedStyleFromAttributes(CSSPropertyID id) override;

 private:
  void ResolveEnableNativeList();
  void ResolvePlatformNodeTag();
  void ResolveEnableDecoupledList();
  bool NeedAsyncResolveListItem();
  bool UseDecoupledList() const;
  bool UseInternalList() const;
  void SetListOrientation(starlight::LinearOrientationType orientation);
  list::BatchRenderStrategy
  ResolveBatchRenderStrategyFromPipelineSchedulerConfig(
      uint64_t pipeline_scheduler_config, bool enable_parallel_element);

 private:
  bool continuous_resolve_tree_{false};
  tasm::TemplateAssembler* tasm_{nullptr};
  lepus::Value component_at_index_{};
  lepus::Value enqueue_component_{};
  lepus::Value component_at_indexes_{};
  std::optional<bool> disable_list_platform_implementation_;
  std::optional<bool> enable_decoupled_list_;
  base::String platform_node_tag_{BASE_STATIC_STRING(kListNodeTag)};
  std::optional<ListElementSSRHelper> ssr_helper_;
  ListNativeItemProvider native_item_provider_;
  bool batch_render_strategy_flushed_{false};
  bool enable_native_list_only_from_env_{false};
  std::unique_ptr<ListMediator> list_mediator_{nullptr};
  std::unique_ptr<ListContainerDelegateInternal>
      list_container_delegate_internal_{nullptr};
  base::auto_create_optional<StyleMap> committed_styles_from_attributes_;
  list::BatchRenderStrategy batch_render_strategy_{
      list::BatchRenderStrategy::kDefault};
};

}  // namespace tasm
}  // namespace lynx

#endif  // CORE_RENDERER_DOM_FIBER_LIST_ELEMENT_H_
