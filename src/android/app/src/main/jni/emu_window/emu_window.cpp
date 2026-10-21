// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <android/native_window_jni.h>

#include "common/android/id_cache.h"
#include "common/logging.h"
#include "input_common/drivers/android.h"
#include "input_common/drivers/touch_screen.h"
#include "input_common/drivers/virtual_amiibo.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "jni/emu_window/emu_window.h"
#include "jni/native.h"

namespace {
void ReleaseSurface(ANativeWindow* surface) {
    if (surface != nullptr) {
        ANativeWindow_release(surface);
    }
}
} // Anonymous namespace

void EmuWindow_Android::OnSurfaceChanged(ANativeWindow* surface) {
    if (surface == nullptr) {
        // A null handle means the frontend lost its surface rather than gained one.
        OnSurfaceCleared();
        return;
    }

    ANativeWindow_acquire(surface);
    {
        std::scoped_lock lock{m_surface_mutex};
        ReleaseSurface(std::exchange(m_surface, surface));
        window_info.render_surface = static_cast<void*>(surface);
    }

    m_window_width = ANativeWindow_getWidth(surface);
    m_window_height = ANativeWindow_getHeight(surface);

    // Ensures that we emulate with the correct aspect ratio.
    UpdateCurrentFramebufferLayout(m_window_width, m_window_height);
}

void EmuWindow_Android::OnSurfaceCleared() {
    // The frontend destroyed its Surface. Keeping the handle here would let the present thread
    // rebuild its surface from a window that no longer has a buffer queue.
    std::scoped_lock lock{m_surface_mutex};
    ReleaseSurface(std::exchange(m_surface, nullptr));
    window_info.render_surface = nullptr;
}

void* EmuWindow_Android::GetRenderSurface() const {
    std::scoped_lock lock{m_surface_mutex};
    return static_cast<void*>(m_surface);
}

void* EmuWindow_Android::AcquireRenderSurface() const {
    std::scoped_lock lock{m_surface_mutex};
    // The extra reference keeps the window alive for as long as the caller builds a surface from
    // it, even if the frontend destroys its Surface in the meantime.
    if (m_surface != nullptr) {
        ANativeWindow_acquire(m_surface);
    }
    return static_cast<void*>(m_surface);
}

void EmuWindow_Android::ReleaseRenderSurface(void* surface) const {
    ReleaseSurface(static_cast<ANativeWindow*>(surface));
}

void EmuWindow_Android::OnTouchPressed(int id, float x, float y) {
    const auto [touch_x, touch_y] = MapToTouchScreen(x, y);
    EmulationSession::GetInstance().GetInputSubsystem().GetTouchScreen()->TouchPressed(touch_x,
                                                                                       touch_y, id);
}

void EmuWindow_Android::OnTouchMoved(int id, float x, float y) {
    const auto [touch_x, touch_y] = MapToTouchScreen(x, y);
    EmulationSession::GetInstance().GetInputSubsystem().GetTouchScreen()->TouchMoved(touch_x,
                                                                                     touch_y, id);
}

void EmuWindow_Android::OnTouchReleased(int id) {
    EmulationSession::GetInstance().GetInputSubsystem().GetTouchScreen()->TouchReleased(id);
}

void EmuWindow_Android::OnFrameDisplayed() {
    if (!m_first_frame) {
        Common::Android::RunJNIOnFiber<void>(
            [&](JNIEnv* env) { EmulationSession::GetInstance().OnEmulationStarted(); });
        m_first_frame = true;
    }
}

EmuWindow_Android::EmuWindow_Android(ANativeWindow* surface,
                                     std::shared_ptr<Common::DynamicLibrary> driver_library)
    : m_driver_library{driver_library} {
    LOG_INFO(Frontend, "initializing");

    window_info.type = Core::Frontend::WindowSystemType::Android;

    if (!surface) {
        LOG_CRITICAL(Frontend, "surface is nullptr");
        return;
    }

    OnSurfaceChanged(surface);
}

EmuWindow_Android::~EmuWindow_Android() {
    OnSurfaceCleared();
}
