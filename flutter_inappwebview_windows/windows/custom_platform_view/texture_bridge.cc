#include "texture_bridge.h"

#include <windows.foundation.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <iostream>

#include "util/direct3d11.interop.h"

namespace flutter_inappwebview_plugin
{
  // [PERF-LOG] kNumBuffers=1: single capture buffer. If Flutter's raster thread holds
  // the mutex in GetSurfaceDescriptor while a new WebView2 frame arrives, the frame pool
  // has nowhere to queue it — causing a drop or stall.
  const int kNumBuffers = 1;

  TextureBridge::TextureBridge(GraphicsContext* graphics_context,
    ABI::Windows::UI::Composition::IVisual* visual)
    : graphics_context_(graphics_context)
  {
    capture_item_ =
      graphics_context_->CreateGraphicsCaptureItemFromVisual(visual);
    assert(capture_item_);

    capture_item_->add_Closed(
      Microsoft::WRL::Callback<ABI::Windows::Foundation::ITypedEventHandler<
      ABI::Windows::Graphics::Capture::GraphicsCaptureItem*,
      IInspectable*>>(
        [](ABI::Windows::Graphics::Capture::IGraphicsCaptureItem* item,
          IInspectable* args) -> HRESULT
        {
          std::cerr << "Capture item was closed." << std::endl;
          return S_OK;
        })
      .Get(),
          &on_closed_token_);
  }

  TextureBridge::~TextureBridge()
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    StopInternal();
    if (capture_item_) {
      capture_item_->remove_Closed(on_closed_token_);
    }
  }

  bool TextureBridge::Start()
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (is_running_ || !capture_item_) {
      return false;
    }

    ABI::Windows::Graphics::SizeInt32 size;
    capture_item_->get_Size(&size);

    frame_pool_ = graphics_context_->CreateCaptureFramePool(
      graphics_context_->device(),
      static_cast<ABI::Windows::Graphics::DirectX::DirectXPixelFormat>(
        kPixelFormat),
      kNumBuffers, size);
    assert(frame_pool_);

    frame_pool_->add_FrameArrived(
      Microsoft::WRL::Callback<ABI::Windows::Foundation::ITypedEventHandler<
      ABI::Windows::Graphics::Capture::Direct3D11CaptureFramePool*,
      IInspectable*>>(
        [this](ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool*
          pool,
          IInspectable* args) -> HRESULT
        {
          OnFrameArrived();
          return S_OK;
        })
      .Get(),
          &on_frame_arrived_token_);

    if (FAILED(frame_pool_->CreateCaptureSession(capture_item_.get(),
      capture_session_.put()))) {
      std::cerr << "Creating capture session failed." << std::endl;
      return false;
    }

    if (SUCCEEDED(capture_session_->StartCapture())) {
      is_running_ = true;
      return true;
    }

    return false;
  }

  void TextureBridge::Stop()
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    StopInternal();
  }

  void TextureBridge::StopInternal()
  {
    if (is_running_) {
      is_running_ = false;
      frame_pool_->remove_FrameArrived(on_frame_arrived_token_);
      auto closable =
        capture_session_.try_as<ABI::Windows::Foundation::IClosable>();
      assert(closable);
      closable->Close();
      capture_session_ = nullptr;
    }
  }

  void TextureBridge::OnFrameArrived()
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!is_running_) {
      return;
    }

#ifndef NDEBUG
    // [PERF-LOG] Log which thread FrameArrived fires on.
    // CreateCaptureFramePool → fires on UI thread (bad: competes with Flutter rendering).
    // CreateFreeThreadedCaptureFramePool → fires on a thread pool thread (better).
    static DWORD s_firstThreadId = 0;
    static std::atomic<uint64_t> s_frameCount{ 0 };
    static std::atomic<uint64_t> s_dropCount{ 0 };
    DWORD currentThreadId = GetCurrentThreadId();
    uint64_t frameNum = ++s_frameCount;

    if (s_firstThreadId == 0) {
      s_firstThreadId = currentThreadId;
      OutputDebugStringA(("[PERF] OnFrameArrived: first frame on threadId=" + std::to_string(currentThreadId) +
        " tick=" + std::to_string(GetTickCount64()) + "\n").c_str());
    }
    else if (currentThreadId != s_firstThreadId) {
      OutputDebugStringA(("[PERF] OnFrameArrived: thread changed! now=" + std::to_string(currentThreadId) +
        " was=" + std::to_string(s_firstThreadId) + " frame=" + std::to_string(frameNum) + "\n").c_str());
      s_firstThreadId = currentThreadId;
    }
#endif

    bool has_frame = false;

    winrt::com_ptr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame>
      frame;
    auto hr = frame_pool_->TryGetNextFrame(frame.put());
    if (SUCCEEDED(hr) && frame) {
      winrt::com_ptr<
        ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface>
        frame_surface;

      if (SUCCEEDED(frame->get_Surface(frame_surface.put()))) {
        last_frame_ =
          TryGetDXGIInterfaceFromObject<ID3D11Texture2D>(frame_surface);
        has_frame = !ShouldDropFrame();
#ifndef NDEBUG
        if (!has_frame) {
          static std::atomic<uint64_t> s_dropCount2{ 0 };
          uint64_t drops = ++s_dropCount2;
          if (drops % 10 == 1) {
            OutputDebugStringA(("[PERF] OnFrameArrived: FPS limiter dropped frame. total_drops=" +
              std::to_string(drops) + "\n").c_str());
          }
        }
#endif
      }
    }
#ifndef NDEBUG
    else {
      // TryGetNextFrame returned nothing — pool may be saturated (kNumBuffers=1)
      static std::atomic<uint64_t> s_emptyCount{ 0 };
      uint64_t empties = ++s_emptyCount;
      if (empties % 20 == 1) {
        OutputDebugStringA(("[PERF] OnFrameArrived: TryGetNextFrame empty (pool saturated?). total_empty=" +
          std::to_string(empties) + "\n").c_str());
      }
    }
#endif

    if (needs_update_) {
      ABI::Windows::Graphics::SizeInt32 size;
      capture_item_->get_Size(&size);
#ifndef NDEBUG
      // [PERF-LOG] Recreate interrupts capture briefly — visible as a glitch.
      OutputDebugStringA(("[PERF] OnFrameArrived: frame_pool_->Recreate (size changed) new=" +
        std::to_string(size.Width) + "x" + std::to_string(size.Height) +
        " tick=" + std::to_string(GetTickCount64()) + "\n").c_str());
#endif
      frame_pool_->Recreate(
        graphics_context_->device(),
        static_cast<ABI::Windows::Graphics::DirectX::DirectXPixelFormat>(
          kPixelFormat),
        kNumBuffers, size);
      needs_update_ = false;
    }

    if (has_frame && frame_available_) {
      frame_available_();
    }
  }

  bool TextureBridge::ShouldDropFrame()
  {
    if (!frame_duration_.has_value()) {
      return false;
    }
    auto now = std::chrono::high_resolution_clock::now();

    bool should_drop_frame = false;
    if (last_frame_timestamp_.has_value()) {
      auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_frame_timestamp_.value());
      should_drop_frame = diff < frame_duration_.value();
    }

    if (!should_drop_frame) {
      last_frame_timestamp_ = now;
    }
    return should_drop_frame;
  }

  void TextureBridge::NotifySurfaceSizeChanged()
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    needs_update_ = true;
  }

  void TextureBridge::SetFpsLimit(std::optional<int> max_fps)
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    auto value = max_fps.value_or(0);
    if (value != 0) {
      frame_duration_ = FrameDuration(1000.0 / value);
    }
    else {
      frame_duration_.reset();
      last_frame_timestamp_.reset();
    }
  }
}