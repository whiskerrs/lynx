// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.
//
// Native renderer C API for embedders that build the element tree
// directly from a host language (e.g. Rust via Whisker), bypassing
// ReactLynx / Lepus template loading entirely.
//
// Usage pattern:
//
//   1. Host obtains a LynxView (Java on Android, Obj-C on iOS) and
//      passes the platform pointer to lynx_shell_from_view().
//   2. Host calls lynx_shell_run_on_tasm_thread() to schedule work
//      on the TASM thread; everything below must run inside that
//      callback.
//   3. Host creates fiber elements via lynx_create_fiber_*(),
//      composes them with lynx_element_append_child(), and registers
//      the root with lynx_shell_set_root_element().
//   4. lynx_shell_flush() commits the tree to layout + paint.
//   5. lynx_element_release() / lynx_shell_release() drop the
//      strong references when done.
//
// All handles are opaque — internally they wrap a LynxShell* / an
// fml::RefPtr<FiberElement> respectively. The C++ types are hidden
// behind the C ABI so the embedder is insulated from name-mangling
// drift across compiler versions.

#ifndef CORE_NATIVE_RENDERER_CAPI_PUBLIC_LYNX_NATIVE_RENDERER_CAPI_H_
#define CORE_NATIVE_RENDERER_CAPI_PUBLIC_LYNX_NATIVE_RENDERER_CAPI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Self-contained export macros. The rest of Lynx uses
// `platform/embedder/public/capi/lynx_export.h` for the same purpose,
// but this header has to compile in embedder builds (Whisker) where
// `platform/embedder/` isn't on the include path — the prebuilt
// LynxAndroid AAR / Lynx.xcframework only ship the `core/`, `base/`,
// and `service_api/` subtrees. Keeping the macros inline avoids any
// cross-subtree include from a public header.
#if defined(__GNUC__) || defined(__clang__)
#define LYNX_NATIVE_RENDERER_CAPI_EXPORT \
  __attribute__((visibility("default")))
#else
#define LYNX_NATIVE_RENDERER_CAPI_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ----- ABI versioning -------------------------------------------------------

// Returns the C ABI version this Lynx build exposes. Embedders that
// resolve the surface dynamically (dlopen + dlsym, e.g. Whisker) call
// this immediately after loading and refuse to proceed on a version
// they weren't compiled against.
//
// The version is bumped whenever the C ABI changes incompatibly — a
// signature change, a function removal, a struct layout change, or
// a behavioural contract change. Pure additions (new functions
// appended at the end of the header) don't require a bump; embedders
// detect missing additions by their own dlsym returning NULL.
//
// Current version: 1
LYNX_NATIVE_RENDERER_CAPI_EXPORT int32_t lynx_capi_abi_version(void);

// ----- Opaque handle types --------------------------------------------------

typedef struct lynx_shell_t lynx_shell_t;
typedef struct lynx_fiber_element_t lynx_fiber_element_t;

// ----- Element tag enum -----------------------------------------------------
//
// Mirrors the subset of Lynx's built-in fiber element types Whisker's
// native renderer use case currently exercises. Adding a new tag here
// is the supported way to expose more element kinds.

typedef enum lynx_element_tag_e {
  LYNX_ELEMENT_TAG_PAGE = 0,
  LYNX_ELEMENT_TAG_VIEW = 1,
  LYNX_ELEMENT_TAG_TEXT = 2,
  LYNX_ELEMENT_TAG_RAW_TEXT = 3,
  LYNX_ELEMENT_TAG_IMAGE = 4,
  LYNX_ELEMENT_TAG_SCROLL_VIEW = 5,
} lynx_element_tag_e;

// ----- Shell wrapping + lifecycle -------------------------------------------

// Wrap a raw native shell pointer (already extracted by the embedder
// from a Java or Obj-C LynxView) into an opaque handle.
//
// `native_shell_ptr` is what `LynxTemplateRender.mNativePtr` holds on
// the Java side (a `jlong` cast back to a pointer) or what
// `LynxView`'s `_shell_` ivar holds on the Obj-C side. Doing the JNI
// reflection / ivar dance stays on the embedder side so this header
// is platform-independent and never needs JNIEnv.
//
// Returns NULL if `native_shell_ptr` is NULL. The handle wraps but
// does NOT own the underlying shell — callers must not outlive the
// LynxView that created it. Pair with lynx_shell_release() when done.
LYNX_NATIVE_RENDERER_CAPI_EXPORT lynx_shell_t* lynx_shell_from_native_ptr(
    void* native_shell_ptr);

LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_shell_release(lynx_shell_t* shell);

// ----- Thread dispatch ------------------------------------------------------

typedef void (*lynx_tasm_callback_t)(void* user_data);

// Schedule `callback(user_data)` to run on the shell's TASM thread.
// On first call, the shell is configured for fiber-arch mode and the
// ElementManager becomes available; subsequent calls just dispatch.
// Returns false if the shell is null or the callback is null.
LYNX_NATIVE_RENDERER_CAPI_EXPORT bool lynx_shell_run_on_tasm_thread(
    lynx_shell_t* shell,
    lynx_tasm_callback_t callback,
    void* user_data);

// ----- Element creation -----------------------------------------------------

// Create a fiber element of the given tag. Must be called from within
// a callback dispatched via lynx_shell_run_on_tasm_thread (the
// ElementManager isn't guaranteed alive otherwise). Returns NULL on
// failure or if the tag is unknown.
//
// The returned handle owns one strong reference; release with
// lynx_element_release() when the embedder no longer needs it.
LYNX_NATIVE_RENDERER_CAPI_EXPORT lynx_fiber_element_t* lynx_create_fiber_element(
    lynx_shell_t* shell,
    lynx_element_tag_e tag);

// Create a fiber element by string tag name. Lets embedders allocate
// custom Lynx elements (any tag registered against Lynx's behaviour
// registry — e.g. `"x-input"`, `"x-refresh"`, third-party-registered
// elements) without going through the closed `lynx_element_tag_e`
// enum. Returns NULL if the tag isn't registered or `tag_name` is
// NULL / empty.
//
// Must be called from within a callback dispatched via
// lynx_shell_run_on_tasm_thread, same as the enum-tag variant. The
// returned handle has the same ownership semantics as
// `lynx_create_fiber_element`'s — caller releases via
// `lynx_element_release`.
LYNX_NATIVE_RENDERER_CAPI_EXPORT lynx_fiber_element_t*
lynx_create_fiber_element_by_name(lynx_shell_t* shell, const char* tag_name);

LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_element_release(lynx_fiber_element_t* element);

// Return the stable per-element id (Lynx calls this `impl_id`).
// Useful for routing events back from the platform event emitter to
// the embedder's registry. Returns 0 if `element` is NULL.
LYNX_NATIVE_RENDERER_CAPI_EXPORT int32_t lynx_element_id(lynx_fiber_element_t* element);

// ----- Element manipulation -------------------------------------------------

// Set a string-valued attribute. UTF-8. Both `key` and `value` must
// be non-null.
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_element_set_attribute(
    lynx_fiber_element_t* element,
    const char* key,
    const char* value);

// Set a number- or bool-valued attribute. The Lynx prop dispatch on
// many UIs (e.g. `<list>`) gates branches on `value.IsNumber()` /
// `value.IsBool()` against the lepus value, so the string-typed
// `lynx_element_set_attribute` silently no-ops for those props. These
// variants wrap the value in a typed `lepus::Value` so the dispatch
// takes the right branch. `key` must be non-null.
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_element_set_attribute_int(
    lynx_fiber_element_t* element,
    const char* key,
    int64_t value);
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_element_set_attribute_bool(
    lynx_fiber_element_t* element,
    const char* key,
    bool value);
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_element_set_attribute_double(
    lynx_fiber_element_t* element,
    const char* key,
    double value);
// Set `key` to an object attribute `{ obj_keys[i]: obj_values[i] }` of
// doubles — for props that read an object value (e.g. `<list>`'s
// `item-snap` → `{factor, offset}`). The scalar setters above cannot
// express a Map value.
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_element_set_attribute_object(
    lynx_fiber_element_t* element,
    const char* key,
    const char* const* obj_keys,
    const double* obj_values,
    int32_t obj_count);

// Set raw inline CSS (as if `style="..."` were declared in template).
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_element_set_inline_styles(
    lynx_fiber_element_t* element,
    const char* css);

// Register a (bubble-phase, `bindEvent`) event handler for
// `event_name` on `element`. The embedder may have no JS runtime, so
// the handler function is a sentinel — the actual fire is observed via
// the event-reporter hook. The point of registering it is to populate
// the element's event set: Lynx's UI components only EMIT their
// component-specific events (scroll, layout, uiappear, …) when the
// element has a handler bound for that event name.
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_element_set_event_handler(
    lynx_fiber_element_t* element,
    const char* event_name);

// Append `child` to `parent`'s children list.
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_element_append_child(
    lynx_fiber_element_t* parent,
    lynx_fiber_element_t* child);

// Remove `child` from `parent`'s children list.
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_element_remove_child(
    lynx_fiber_element_t* parent,
    lynx_fiber_element_t* child);

// ----- List native item provider -------------------------------------------

// Returned by `lynx_list_component_at_index_fn` to signal "no element
// could be produced for this index". Matches `list::kInvalidIndex`.
#define LYNX_LIST_INVALID_INDEX 0

// Called by Lynx's list machinery when it needs the element for a given
// `index`. Implementation must create or look up a child FiberElement
// and return its `lynx_element_id`, or `LYNX_LIST_INVALID_INDEX` on
// failure. `user_data` is the cookie passed to
// `lynx_list_set_native_item_provider`. `reuse_notification` mirrors the
// upstream `enable_reuse_notification` flag (1 if the embedder should
// treat the call as "may reuse an existing element").
typedef int32_t (*lynx_list_component_at_index_fn)(uint32_t index,
                                                    int64_t operation_id,
                                                    int reuse_notification,
                                                    void* user_data);

// Optional. Called when the element at `sign` leaves the viewport, so
// the embedder can pool or release it. May be NULL — recycling
// notifications will then be silently dropped.
typedef void (*lynx_list_enqueue_component_fn)(int32_t sign, void* user_data);

// Free-callback for the `user_data` cookie, invoked when the list
// element is destroyed or the provider is cleared. May be NULL if the
// embedder manages the cookie's lifetime externally.
typedef void (*lynx_user_data_free_fn)(void* user_data);

// Install a native (non-lepus) item provider on a `<list>` element.
// While installed, the list routes its `componentAtIndex` /
// `enqueueComponent` / `componentAtIndexes` calls to these C callbacks
// instead of the lepus framework callbacks — letting an embedder
// without a JS runtime drive the list directly while keeping all of
// Lynx's virtualisation / recycling / layout behaviour.
//
// `element` must have been created with `LYNX_ELEMENT_TAG_LIST` (or
// the `"list"` by-name path). Passing a non-list element is a no-op.
// `component_at_index` is required; `enqueue_component` is optional
// (pass NULL to ignore recycling notifications). `user_data_free` is
// invoked on the cookie when the list is destroyed OR when another
// provider is installed on top — pass NULL if no cleanup is needed.
//
// Calling this again replaces the previous provider (and invokes the
// previous provider's `user_data_free`). Pass `component_at_index =
// NULL` to clear the provider entirely.
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_list_set_native_item_provider(
    lynx_fiber_element_t* element,
    lynx_list_component_at_index_fn component_at_index,
    lynx_list_enqueue_component_fn enqueue_component,
    void* user_data,
    lynx_user_data_free_fn user_data_free);

// Broadcast the item count on a `<list>` element by writing an
// `update-list-info` Map attribute with `count` `insertAction`
// entries (`{position: i, item-key: "w_<i>"}`). The decoupled list
// container routes this attr to `ListAdapter::UpdateFiberDataSource`,
// which then calls back into the installed item provider — typically
// the one set via `lynx_list_set_native_item_provider`. Pair the two
// to drive a virtualised `<list>` from a non-JS embedder.
//
// `update-list-info` is a structured (Map) attribute, so embedders
// without lepus header access can't synthesise it via the string
// `lynx_element_set_attribute` capi — this capi exists so they can
// trigger the broadcast with just an `int`.
// Drive a `<list>`'s decoupled data source. `item_keys[0..count)` are the
// REAL (stable) item-keys in current order; the parallel arrays carry
// per-item layout metadata (`estimated_main_axis_px` uses -1 for "unset";
// the `uint8_t*` flag arrays use 0/1, with `recyclable` defaulting to 1).
// Any metadata array may be null to omit it entirely. `prev_count` is the
// item count from the previous call (0 on first) — the update is a full
// replace (removeAction over the old positions + insertAction of the new
// items), so the native adapter recomputes moves/inserts/removes from the
// keys. Builds a Map-valued `update-list-info` attribute, which the
// string-only `lynx_element_set_attribute` capi cannot express.
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_element_set_update_list_info(
    lynx_fiber_element_t* element,
    int32_t prev_count,
    const char* const* item_keys,
    const int32_t* estimated_main_axis_px,
    const uint8_t* full_span,
    const uint8_t* sticky_top,
    const uint8_t* sticky_bottom,
    const uint8_t* recyclable,
    int32_t count);

// Explicit diff actions for the decoupled `<list>` data source —
// the minimal-action alternative to `lynx_element_set_update_list_info`
// (which sends a full replace and therefore severs every ItemHolder's
// identity, collapsing the scroll anchor to the top on data updates).
// Items mentioned in NEITHER action keep their identity, matching how
// ReactLynx drives the same adapter.
//
// Index contract (what `AdapterHelper` expects):
//   - `remove_indices`: ascending indices into the PRE-update item-key
//     list. All removals apply before any insert.
//   - `insert_positions` / `insert_keys` (parallel, `insert_count`
//     long): ascending splice points into the POST-removal list,
//     applied in array order.
//
// Per-item layout metadata (estimated size / full-span / sticky /
// recyclable) stays on the `<list-item>` elements, as with the
// full-replace entry. Tail addition after ABI v2 — feature-detect via
// dlsym.
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_element_update_list_actions(
    lynx_fiber_element_t* element,
    const int32_t* remove_indices,
    int32_t remove_count,
    const int32_t* insert_positions,
    const char* const* insert_keys,
    int32_t insert_count);

// One list item's action entry for `lynx_element_update_list_actions_v2`:
// the item-key plus the per-item layout metadata the adapter ingests
// from actions (`fiber_full_spans_` / `fiber_sticky_*` /
// `fiber_estimated_sizes_px_` / `fiber_unrecyclable_`). Layout is part
// of the ABI — fields are fixed-width and must not be reordered.
typedef struct lynx_list_item_action_t {
  // Insert: ascending splice position into the post-removal list.
  // Update: the item's index in the FINAL (post-remove+insert) list.
  int32_t position;
  // NUL-terminated UTF-8, borrowed for the duration of the call.
  const char* item_key;
  // Estimated main-axis size in px; < 0 = unset (native default).
  int32_t estimated_main_axis_px;
  // Booleans (0 / 1). For updates these SET the state both ways
  // (true inserts into the adapter's meta set, false erases).
  uint8_t full_span;
  uint8_t sticky_top;
  uint8_t sticky_bottom;
  uint8_t recyclable;
} lynx_list_item_action_t;

// Metadata-carrying successor to `lynx_element_update_list_actions`
// (whose signature is frozen by the ABI contract). Same index
// semantics for removals/inserts; additionally:
//   - insert entries carry the per-item layout metadata, which is the
//     ONLY channel the adapter ingests it from (list-item element
//     attributes are NOT read by the decoupled list);
//   - `updates` refresh the metadata of SURVIVING items in place
//     (emitted as updateAction {from == to, flush: false} — content
//     re-render is the embedder's own reactive concern).
// Tail addition after ABI v2 — feature-detect via dlsym.
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_element_update_list_actions_v2(
    lynx_fiber_element_t* element,
    const int32_t* remove_indices,
    int32_t remove_count,
    const lynx_list_item_action_t* inserts,
    int32_t insert_count,
    const lynx_list_item_action_t* updates,
    int32_t update_count);

// ----- Pipeline -------------------------------------------------------------

// Install `page` as the shell's root PageElement. `page` MUST have
// been created with LYNX_ELEMENT_TAG_PAGE.
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_shell_set_root_element(
    lynx_shell_t* shell,
    lynx_fiber_element_t* page);

// Commit the current element tree — flush fiber actions, run patch
// finalization, schedule layout + paint. Must be called from inside
// the TASM thread callback.
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_shell_flush(lynx_shell_t* shell);

// ----- UI method dispatch ---------------------------------------------------
//
// Invoke a Lynx UI method on a mounted element by sign. Wraps
// `Catalyzer::Invoke` so the call routes through the platform's
// `LynxUIMethodProcessor.invokeMethod:forUI:` (iOS) /
// `LynxUIMethodsExecutor.invokeMethod(...)` (Android) machinery —
// which then dispatches to whichever method is registered on the
// mounted `LynxUI` / `LynxBaseUI` for `sign`.
//
// `sign` is the value returned by `lynx_element_id` for a fiber
// element that has been flushed at least once (so the platform UI
// counterpart actually exists).
//
// Args are encoded as a flat `lynx_ui_method_value_t` array. The
// implementation packages them into the lepus::Value tree
// `{"args": [arg0, arg1, ...]}` that the platform-side
// LynxUIMethodProcessor / LynxUIMethodsExecutor forwarders see
// (matching Whisker's `WhiskerValue.fromNSDictionary` /
// `WhiskerValue.fromReadableMap` decoding convention — Phase
// 7-Φ.H.2 on the embedder side).
//
// Currently fire-and-forget — the underlying platform Invoke
// dispatches the call asynchronously on the main / UI thread, so
// the C wrapper returns immediately. A return value of `0` means
// "dispatch was scheduled successfully"; non-zero indicates the
// preconditions failed (NULL shell, NULL method, manager not
// initialised). Method-side failures (no UI for sign, no such
// method on the UI) surface as JS-side callback errors but are
// invisible to this caller — for v1 the embedder's typed wrappers
// discard return values anyway. An async-result variant can land
// later if a real use case demands it.

typedef enum lynx_ui_method_value_type_e {
  LYNX_UI_METHOD_VALUE_NULL = 0,
  LYNX_UI_METHOD_VALUE_BOOL = 1,
  LYNX_UI_METHOD_VALUE_INT = 2,
  LYNX_UI_METHOD_VALUE_DOUBLE = 3,
  LYNX_UI_METHOD_VALUE_STRING = 4,
  // Recursive variants — used for method *results* (a
  // `boundingClientRect` map, etc.). Args only ever use the scalar
  // variants above.
  LYNX_UI_METHOD_VALUE_ARRAY = 5,
  LYNX_UI_METHOD_VALUE_MAP = 6,
} lynx_ui_method_value_type_e;

// Forward-declared so `array`/`map` can hold them recursively. The
// map holds a *pointer* to `kv` (incomplete-type OK), and `kv` holds
// `value` *by value* — so `kv` is defined AFTER the full
// `lynx_ui_method_value_t` below.
struct lynx_ui_method_value_t;
struct lynx_ui_method_kv_t;

typedef struct lynx_ui_method_value_array_t {
  struct lynx_ui_method_value_t* items;  // length = count
  size_t count;
} lynx_ui_method_value_array_t;

typedef struct lynx_ui_method_value_map_t {
  struct lynx_ui_method_kv_t* entries;  // length = count
  size_t count;
} lynx_ui_method_value_map_t;

typedef struct lynx_ui_method_value_t {
  lynx_ui_method_value_type_e type;
  union {
    bool b;
    int64_t i;
    double f;
    // String: caller-owned UTF-8, NUL-terminated. For args, borrowed
    // for the duration of the call. For results, owned by the wrapper
    // and freed after the result callback returns.
    const char* s;
    lynx_ui_method_value_array_t array;
    lynx_ui_method_value_map_t map;
  } v;
} lynx_ui_method_value_t;

typedef struct lynx_ui_method_kv_t {
  const char* key;  // NUL-terminated UTF-8
  lynx_ui_method_value_t value;
} lynx_ui_method_kv_t;

LYNX_NATIVE_RENDERER_CAPI_EXPORT int32_t lynx_ui_invoke_method(
    lynx_shell_t* shell,
    int32_t sign,
    const char* method_name,
    const lynx_ui_method_value_t* args,
    size_t arg_count);

// Params-map UI-method dispatch (fire-and-forget) — for built-in Lynx
// UI methods (`scrollTo`, `scrollBy`, `autoScroll`, `scrollIntoView`,
// `requestUIInfo`, ...) that read their arguments as *named fields* of
// the params object rather than from the `{"args": [...]}` wrapper
// `lynx_ui_invoke_method` builds. `params` must be a single MAP value;
// it's passed through as the params object directly (nested maps /
// arrays round-trip). A null / non-map `params` degrades to an empty
// object so the platform method runs with its defaults.
LYNX_NATIVE_RENDERER_CAPI_EXPORT int32_t lynx_ui_invoke_method_with_params(
    lynx_shell_t* shell,
    int32_t sign,
    const char* method_name,
    const lynx_ui_method_value_t* params);

// Async UI-method dispatch — the result-returning variant used for
// `boundingClientRect` / `takeScreenshot` etc. Unlike the
// fire-and-forget `lynx_ui_invoke_method`, this captures the
// `Catalyzer::Invoke` result callback: it converts the callback's
// `lynx::pub::Value` into a heap-owned `lynx_ui_method_value_t` tree
// and invokes `callback(code, &result, user_data)` (typically on the
// UI thread, after the method runs). `result` is owned by the wrapper
// and only valid for the duration of the callback — the wrapper frees
// the tree once `callback` returns, so the callee must copy out.
typedef void (*lynx_ui_method_result_cb)(int32_t code,
                                          const lynx_ui_method_value_t* result,
                                          void* user_data);
LYNX_NATIVE_RENDERER_CAPI_EXPORT int32_t lynx_ui_invoke_method_async(
    lynx_shell_t* shell,
    int32_t sign,
    const char* method_name,
    const lynx_ui_method_value_t* args,
    size_t arg_count,
    lynx_ui_method_result_cb callback,
    void* user_data);

// Unified params-map + result dispatch — `params` (a single MAP value)
// is passed through as the params object directly (no `{"args": [...]}`
// wrapper; the caller builds named fields for built-in methods, or an
// `{"args": [...]}` map for Whisker module elements), and the result
// arrives via `callback`. This is the one capi the Whisker
// `ElementRef::invoke` family builds on, so adding a new built-in /
// module method never needs a new capi. A null / non-map `params`
// degrades to an empty object.
LYNX_NATIVE_RENDERER_CAPI_EXPORT int32_t lynx_ui_invoke_method_async_with_params(
    lynx_shell_t* shell,
    int32_t sign,
    const char* method_name,
    const lynx_ui_method_value_t* params,
    lynx_ui_method_result_cb callback,
    void* user_data);

// ----- Element-level animation dispatch -------------------------------------
//
// Exposes `Element::Animate` (DOM layer, distinct from `lynx_ui_invoke_method`
// which targets the UI layer below). Mirrors the JS `element.animate(...)`
// shape from `runtime/js/bindings/java_script_element.cc`:
//
//     [operation, animation_name, keyframes_map, options_map]
//
// `operation` follows `JavaScriptElement::AnimationOperation`:
//   0 = START, 1 = PLAY, 2 = PAUSE, 3 = CANCEL, 4 = FINISH
//
// For PLAY / PAUSE / CANCEL / FINISH only `animation_name` is required; pass
// NULL for `keyframes` and `options`.
//
// For START all four are required:
//   - `animation_name` — string identifier (used as the keyframes-map key)
//   - `keyframes` — MAP with `"0%" / "50%" / "100%"` keys → MAP of CSS props
//   - `options` — MAP of `name`, `duration`, `easing`, `iterations`,
//                 `direction`, `fill`, `delay`, `play-state`
//
// Returns 0 on dispatch success; non-zero on precondition failure (NULL
// shell / element / element not flushed). Method-side errors (bad keyframes,
// invalid CSS values) are logged by Lynx and do not surface here — v1
// contract.
LYNX_NATIVE_RENDERER_CAPI_EXPORT int32_t lynx_element_animate(
    lynx_shell_t* shell,
    lynx_fiber_element_t* element,
    int32_t operation,
    const char* animation_name,
    const lynx_ui_method_value_t* keyframes,
    const lynx_ui_method_value_t* options);

// ----- Core-originated custom events -----------------------------------------
//
// Some component events are generated inside the engine core rather
// than by the platform UI layer — today that is the `<list>` family
// (`scroll` / `scrolltoupper` / `scrolltolower` / `snap` /
// `layoutcomplete` / impression events) plus `<frame>` events. Those
// events are dispatched to the JS event system only, so an embedder
// without a JS runtime never sees them (the platform event-reporter
// hook is NOT on their path).
//
// Registering this callback routes every core-originated custom event
// to the embedder instead. Contract:
//   - `element_id` is the target's `impl_id` — the same id space
//     `lynx_element_get_id` returns.
//   - `params` is the event payload (what JS would receive as
//     `detail`), encoded as a `lynx_ui_method_value_t` tree. It is
//     only valid for the duration of the call — deep-copy to retain.
//     May be NULL when the event carries no payload.
//   - The callback is invoked synchronously on the engine (TASM)
//     thread from within the send path. Do not re-enter the engine;
//     hand off to your own thread/queue for real work.
//   - Return true to consume the event (it will NOT be forwarded to
//     the JS event system); false to observe-and-forward.
//
// Platform-originated events (touch/gesture, platform-emitted
// component events such as `<scroll-view>` scroll) are unaffected —
// they keep flowing through the platform event-reporter hook.
//
// Pass a NULL `callback` to unregister. Must be called on the TASM
// thread (e.g. via `lynx_shell_run_on_tasm_thread`) after fiber-arch
// init, like the other element APIs.
typedef bool (*lynx_custom_event_callback_t)(
    void* user_data,
    int32_t element_id,
    const char* event_name,
    const lynx_ui_method_value_t* params);

LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_shell_set_custom_event_callback(
    lynx_shell_t* shell,
    lynx_custom_event_callback_t callback,
    void* user_data);

// ----- subsecond ASLR anchor ------------------------------------------------

// No-op function whose address serves as a well-known anchor for
// subsecond-style hot-patch frameworks (Whisker) to compute the
// runtime ASLR slide for liblynx.so. Embedders that don't use
// subsecond can ignore this; calling it has no side effects.
//
// Stable across Lynx versions by contract — DO NOT remove or
// rename. This symbol exists *because* a hot-patch framework needs
// some reliably-exported Lynx symbol to dlsym against.
LYNX_NATIVE_RENDERER_CAPI_EXPORT void lynx_aslr_reference(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // CORE_NATIVE_RENDERER_CAPI_PUBLIC_LYNX_NATIVE_RENDERER_CAPI_H_
