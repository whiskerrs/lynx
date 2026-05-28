// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#define private public
#define protected public

#include "core/renderer/dom/fiber/list_element.h"

#include "core/renderer/tasm/react/testing/mock_painting_context.h"
#include "core/shell/testing/mock_tasm_delegate.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace lynx {
namespace tasm {
namespace testing {

static constexpr int32_t kWidth = 1080;
static constexpr int32_t kHeight = 1920;
static constexpr float kDefaultLayoutsUnitPerPx = 1.f;
static constexpr double kDefaultPhysicalPixelsPerLayoutUnit = 1.f;

class SSRListElement : public ::testing::Test {
 public:
  SSRListElement() = default;
  ~SSRListElement() override = default;

  fml::RefPtr<PageElement> page_;
  fml::RefPtr<ListElement> list_element_;

  std::shared_ptr<::testing::NiceMock<test::MockTasmDelegate>> tasm_mediator_;
  std::shared_ptr<lynx::tasm::TemplateAssembler> tasm_;
  ElementManager* manager_ = nullptr;

  void SetUp() override {
    LynxEnvConfig lynx_env_config(kWidth, kHeight, kDefaultLayoutsUnitPerPx,
                                  kDefaultPhysicalPixelsPerLayoutUnit);
    tasm_mediator_ = std::make_shared<
        ::testing::NiceMock<lynx::tasm::test::MockTasmDelegate>>();
    auto manager = std::make_unique<lynx::tasm::ElementManager>(
        std::make_unique<MockPaintingContext>(), tasm_mediator_.get(),
        lynx_env_config);
    auto config = std::make_shared<PageConfig>();
    config->SetEnableFiberArch(true);
    manager->SetConfig(config);
    manager_ = manager.get();
    tasm_ = std::make_shared<lynx::tasm::TemplateAssembler>(
        *tasm_mediator_.get(), std::move(manager), tasm_mediator_.get(), 0);

    page_ = fml::AdoptRef<PageElement>(new PageElement(manager_, "page", 0));
    list_element_ = fml::AdoptRef<ListElement>(new ListElement(
        manager_, "list", lepus::Value(), lepus::Value(), lepus::Value()));
    page_->InsertNode(list_element_);
    list_element_->set_tasm(tasm_.get());
  }
};

TEST_F(SSRListElement, AttributeStyleCacheMirrorsCommittedStyleCache) {
  list_element_->CacheStyleFromAttributes(CSSPropertyID::kPropertyIDWidth,
                                          CSSValue(120, CSSValuePattern::PX));

  const auto* cached_styles = list_element_->PeekCachedStylesFromAttributes();
  ASSERT_NE(cached_styles, nullptr);
  auto cached_it = cached_styles->find(CSSPropertyID::kPropertyIDWidth);
  ASSERT_TRUE(cached_it != cached_styles->end());
  EXPECT_EQ(cached_it->second, CSSValue(120, CSSValuePattern::PX));

  const auto* committed_styles =
      list_element_->PeekCommittedStylesFromAttributes();
  ASSERT_NE(committed_styles, nullptr);
  auto committed_it = committed_styles->find(CSSPropertyID::kPropertyIDWidth);
  ASSERT_TRUE(committed_it != committed_styles->end());
  EXPECT_EQ(committed_it->second, CSSValue(120, CSSValuePattern::PX));

  list_element_->RemoveStyleFromAttributes(CSSPropertyID::kPropertyIDWidth);
  EXPECT_EQ(list_element_->PeekCachedStylesFromAttributes(), nullptr);
  EXPECT_EQ(list_element_->PeekCommittedStylesFromAttributes(), nullptr);
}

TEST_F(SSRListElement, ScrollOrientationAttributeUsesAttributeStyleCache) {
  list_element_->SetAttributeInternal(base::String("scroll-orientation"),
                                      lepus::Value("horizontal"));

  const auto* cached_styles = list_element_->PeekCachedStylesFromAttributes();
  ASSERT_NE(cached_styles, nullptr);
  auto cached_it =
      cached_styles->find(CSSPropertyID::kPropertyIDLinearOrientation);
  ASSERT_TRUE(cached_it != cached_styles->end());
  EXPECT_EQ(cached_it->second,
            CSSValue(starlight::LinearOrientationType::kHorizontal));

  const auto* committed_styles =
      list_element_->PeekCommittedStylesFromAttributes();
  ASSERT_NE(committed_styles, nullptr);
  auto committed_it =
      committed_styles->find(CSSPropertyID::kPropertyIDLinearOrientation);
  ASSERT_TRUE(committed_it != committed_styles->end());
  EXPECT_EQ(committed_it->second,
            CSSValue(starlight::LinearOrientationType::kHorizontal));
  EXPECT_TRUE(list_element_->parsed_styles_map_.find(
                  CSSPropertyID::kPropertyIDLinearOrientation) ==
              list_element_->parsed_styles_map_.end());
}

TEST_F(SSRListElement, ListElementSSRHelper_ComponentAtIndexInSSR) {
  auto items = std::vector<fml::RefPtr<FiberElement>>();
  ListElementSSRHelper ssr_helper(list_element_.get());

  static const size_t kItemCounts = 10;
  for (size_t index = 0; index < kItemCounts; index++) {
    auto item = fml::AdoptRef<ComponentElement>(
        new ComponentElement(manager_, "", 1, "", "", ""));
    ssr_helper.AppendChild(item);
    items.emplace_back(item);
  }

  // ssr list init
  list_element_->SetSsrHelper(std::move(ssr_helper));
  for (size_t index = 0; index < kItemCounts; index++) {
    EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kWaitingRender ==
                list_element_->ssr_helper_->ssr_elements_[index].second);
  }
  EXPECT_TRUE(0 == list_element_->children().size());
  EXPECT_FALSE(list_element_->element_manager() == nullptr);
  // list first screen.
  list_element_->ComponentAtIndex(0, -1, false);
  list_element_->ComponentAtIndex(1, -1, false);
  list_element_->ComponentAtIndex(2, -1, false);

  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kRendered ==
              list_element_->ssr_helper_->ssr_elements_[0].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kRendered ==
              list_element_->ssr_helper_->ssr_elements_[1].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kRendered ==
              list_element_->ssr_helper_->ssr_elements_[2].second);
  EXPECT_TRUE(3 == list_element_->children().size());

  // list scroll down.
  list_element_->EnqueueComponent(items[0]->impl_id());
  list_element_->ComponentAtIndex(3, -1, false);
  list_element_->EnqueueComponent(items[1]->impl_id());
  list_element_->ComponentAtIndex(4, -1, false);
  list_element_->EnqueueComponent(items[2]->impl_id());
  list_element_->ComponentAtIndex(5, -1, false);

  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kEnqueued ==
              list_element_->ssr_helper_->ssr_elements_[0].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kEnqueued ==
              list_element_->ssr_helper_->ssr_elements_[1].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kEnqueued ==
              list_element_->ssr_helper_->ssr_elements_[2].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kRendered ==
              list_element_->ssr_helper_->ssr_elements_[3].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kRendered ==
              list_element_->ssr_helper_->ssr_elements_[4].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kRendered ==
              list_element_->ssr_helper_->ssr_elements_[5].second);
  EXPECT_TRUE(6 == list_element_->children().size());

  // list scroll up.
  list_element_->EnqueueComponent(items[5]->impl_id());
  list_element_->ComponentAtIndex(2, -1, false);
  list_element_->EnqueueComponent(items[4]->impl_id());
  list_element_->ComponentAtIndex(1, -1, false);

  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kEnqueued ==
              list_element_->ssr_helper_->ssr_elements_[0].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kRendered ==
              list_element_->ssr_helper_->ssr_elements_[1].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kRendered ==
              list_element_->ssr_helper_->ssr_elements_[2].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kRendered ==
              list_element_->ssr_helper_->ssr_elements_[3].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kEnqueued ==
              list_element_->ssr_helper_->ssr_elements_[4].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kEnqueued ==
              list_element_->ssr_helper_->ssr_elements_[5].second);
  EXPECT_TRUE(6 == list_element_->children().size());

  // hydrate
  list_element_->Hydrate();
  EXPECT_TRUE(kItemCounts == list_element_->children().size());

  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kEnqueued ==
              list_element_->ssr_helper_->ssr_elements_[0].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kRendered ==
              list_element_->ssr_helper_->ssr_elements_[1].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kRendered ==
              list_element_->ssr_helper_->ssr_elements_[2].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kRendered ==
              list_element_->ssr_helper_->ssr_elements_[3].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kEnqueued ==
              list_element_->ssr_helper_->ssr_elements_[4].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kEnqueued ==
              list_element_->ssr_helper_->ssr_elements_[5].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kEnqueued ==
              list_element_->ssr_helper_->ssr_elements_[6].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kEnqueued ==
              list_element_->ssr_helper_->ssr_elements_[7].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kEnqueued ==
              list_element_->ssr_helper_->ssr_elements_[8].second);
  EXPECT_TRUE(ListElementSSRHelper::SSRItemStatus::kEnqueued ==
              list_element_->ssr_helper_->ssr_elements_[9].second);
}

// ---------------------------------------------------------------------------
// Native item provider tests
// ---------------------------------------------------------------------------

TEST_F(SSRListElement, NativeItemProvider_HasAndClear) {
  EXPECT_FALSE(list_element_->HasNativeItemProvider());

  ListNativeItemProvider provider;
  provider.component_at_index = [](uint32_t, int64_t, bool) -> int32_t {
    return 0;
  };
  list_element_->SetNativeItemProvider(std::move(provider));
  EXPECT_TRUE(list_element_->HasNativeItemProvider());

  list_element_->ClearNativeItemProvider();
  EXPECT_FALSE(list_element_->HasNativeItemProvider());
}

TEST_F(SSRListElement, NativeItemProvider_ComponentAtIndexRoutesToCallback) {
  // Track every call.
  std::vector<std::tuple<uint32_t, int64_t, bool>> calls;
  ListNativeItemProvider provider;
  provider.component_at_index = [&calls](uint32_t index, int64_t op_id,
                                          bool reuse) -> int32_t {
    calls.emplace_back(index, op_id, reuse);
    // Return a synthetic impl_id derived from index so the test can
    // see the value flow back through ListElement::ComponentAtIndex.
    return static_cast<int32_t>(1000 + index);
  };
  list_element_->SetNativeItemProvider(std::move(provider));

  EXPECT_EQ(1000, list_element_->ComponentAtIndex(0, 42, false));
  EXPECT_EQ(1003, list_element_->ComponentAtIndex(3, 43, true));

  ASSERT_EQ(2u, calls.size());
  EXPECT_EQ(0u, std::get<0>(calls[0]));
  EXPECT_EQ(42, std::get<1>(calls[0]));
  EXPECT_FALSE(std::get<2>(calls[0]));
  EXPECT_EQ(3u, std::get<0>(calls[1]));
  EXPECT_EQ(43, std::get<1>(calls[1]));
  EXPECT_TRUE(std::get<2>(calls[1]));
}

TEST_F(SSRListElement, NativeItemProvider_EnqueueRoutesWhenProvided) {
  std::vector<int32_t> enqueued;
  ListNativeItemProvider provider;
  provider.component_at_index = [](uint32_t, int64_t, bool) -> int32_t {
    return 0;
  };
  provider.enqueue_component = [&enqueued](int32_t sign) {
    enqueued.push_back(sign);
  };
  list_element_->SetNativeItemProvider(std::move(provider));

  list_element_->EnqueueComponent(17);
  list_element_->EnqueueComponent(42);

  ASSERT_EQ(2u, enqueued.size());
  EXPECT_EQ(17, enqueued[0]);
  EXPECT_EQ(42, enqueued[1]);
}

TEST_F(SSRListElement, NativeItemProvider_EnqueueWithoutCallbackIsNoOp) {
  // `component_at_index` is set but `enqueue_component` is left empty
  // — recycling notifications must be silently dropped instead of
  // falling through to the lepus path (which would crash on the empty
  // `enqueue_component_`).
  ListNativeItemProvider provider;
  provider.component_at_index = [](uint32_t, int64_t, bool) -> int32_t {
    return 0;
  };
  list_element_->SetNativeItemProvider(std::move(provider));
  // No assertion needed beyond "does not crash".
  list_element_->EnqueueComponent(99);
}

TEST_F(SSRListElement,
       NativeItemProvider_ComponentAtIndexesBatchFallsBackToLoop) {
  // Provider supplies only the single-item callback; the list's batch
  // path must loop over it instead of falling through to the lepus
  // `component_at_indexes_` (which is empty in this fixture).
  std::vector<uint32_t> seen_indices;
  std::vector<int64_t> seen_ops;
  ListNativeItemProvider provider;
  provider.component_at_index = [&seen_indices, &seen_ops](
                                    uint32_t index, int64_t op_id,
                                    bool) -> int32_t {
    seen_indices.push_back(index);
    seen_ops.push_back(op_id);
    return static_cast<int32_t>(2000 + index);
  };
  list_element_->SetNativeItemProvider(std::move(provider));

  auto indices = lepus::CArray::Create();
  indices->emplace_back(lepus::Value(static_cast<uint32_t>(2)));
  indices->emplace_back(lepus::Value(static_cast<uint32_t>(5)));
  indices->emplace_back(lepus::Value(static_cast<uint32_t>(7)));
  auto op_ids = lepus::CArray::Create();
  op_ids->emplace_back(lepus::Value(static_cast<int64_t>(101)));
  op_ids->emplace_back(lepus::Value(static_cast<int64_t>(102)));
  op_ids->emplace_back(lepus::Value(static_cast<int64_t>(103)));

  list_element_->ComponentAtIndexes(indices, op_ids, false);

  ASSERT_EQ(3u, seen_indices.size());
  EXPECT_EQ(2u, seen_indices[0]);
  EXPECT_EQ(5u, seen_indices[1]);
  EXPECT_EQ(7u, seen_indices[2]);
  ASSERT_EQ(3u, seen_ops.size());
  EXPECT_EQ(101, seen_ops[0]);
  EXPECT_EQ(102, seen_ops[1]);
  EXPECT_EQ(103, seen_ops[2]);
}

TEST_F(SSRListElement, NativeItemProvider_BatchCallbackUsedWhenProvided) {
  // When both single and batch callbacks are present, the batch one
  // should be invoked directly (no looping into `component_at_index`).
  bool single_called = false;
  std::vector<uint32_t> batch_indices;
  std::vector<int64_t> batch_ops;
  bool batch_reuse = false;
  ListNativeItemProvider provider;
  provider.component_at_index = [&single_called](uint32_t, int64_t,
                                                  bool) -> int32_t {
    single_called = true;
    return 0;
  };
  provider.component_at_indexes =
      [&](const std::vector<uint32_t>& indices,
          const std::vector<int64_t>& op_ids, bool reuse) {
        batch_indices = indices;
        batch_ops = op_ids;
        batch_reuse = reuse;
      };
  list_element_->SetNativeItemProvider(std::move(provider));

  auto indices = lepus::CArray::Create();
  indices->emplace_back(lepus::Value(static_cast<uint32_t>(0)));
  indices->emplace_back(lepus::Value(static_cast<uint32_t>(1)));
  auto op_ids = lepus::CArray::Create();
  op_ids->emplace_back(lepus::Value(static_cast<int64_t>(10)));
  op_ids->emplace_back(lepus::Value(static_cast<int64_t>(20)));

  list_element_->ComponentAtIndexes(indices, op_ids, true);

  EXPECT_FALSE(single_called);
  ASSERT_EQ(2u, batch_indices.size());
  EXPECT_EQ(0u, batch_indices[0]);
  EXPECT_EQ(1u, batch_indices[1]);
  ASSERT_EQ(2u, batch_ops.size());
  EXPECT_EQ(10, batch_ops[0]);
  EXPECT_EQ(20, batch_ops[1]);
  EXPECT_TRUE(batch_reuse);
}

TEST_F(SSRListElement, NativeItemProvider_SSRHelperStillTakesPriority) {
  // If an SSR helper is installed AND a native provider is installed,
  // SSR wins (mirrors the existing `if (ssr_helper_) return` at the
  // top of ComponentAtIndex). Important: SSR hydration must not be
  // disturbed by an embedder accidentally also installing a native
  // provider.
  auto item = fml::AdoptRef<ComponentElement>(
      new ComponentElement(manager_, "", 1, "", "", ""));
  ListElementSSRHelper ssr_helper(list_element_.get());
  ssr_helper.AppendChild(item);
  list_element_->SetSsrHelper(std::move(ssr_helper));

  bool native_called = false;
  ListNativeItemProvider provider;
  provider.component_at_index = [&native_called](uint32_t, int64_t,
                                                  bool) -> int32_t {
    native_called = true;
    return -1;
  };
  list_element_->SetNativeItemProvider(std::move(provider));

  list_element_->ComponentAtIndex(0, -1, false);

  EXPECT_FALSE(native_called);
}

}  // namespace testing
}  // namespace tasm
}  // namespace lynx
