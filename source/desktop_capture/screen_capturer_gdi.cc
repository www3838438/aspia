//
// PROJECT:         Aspia
// FILE:            desktop_capture/screen_capturer_gdi.cc
// LICENSE:         GNU General Public License 3
// PROGRAMMERS:     Dmitry Chapyshev (dmitry@aspia.ru)
//

#include "desktop_capture/screen_capturer_gdi.h"

#include <QDebug>
#include <dwmapi.h>

#include "desktop_capture/win/screen_capture_utils.h"

namespace aspia {

bool ScreenCapturerGDI::prepareCaptureResources()
{
    // Switch to the desktop receiving user input if different from the
    // current one.
    Desktop input_desktop(Desktop::inputDesktop());

    if (input_desktop.isValid() && !desktop_.isSame(input_desktop))
    {
        // Release GDI resources otherwise SetThreadDesktop will fail.
        desktop_dc_.reset();
        memory_dc_.reset();

        // If SetThreadDesktop() fails, the thread is still assigned a desktop.
        // So we can continue capture screen bits, just from the wrong desktop.
        desktop_.setThreadDesktop(std::move(input_desktop));
    }

    QRect screen_rect = fullScreenRect();

    // If the display bounds have changed then recreate GDI resources.
    if (screen_rect != desktop_dc_rect_)
    {
        desktop_dc_.reset();
        memory_dc_.reset();

        desktop_dc_rect_ = QRect();
    }

    if (!desktop_dc_)
    {
        Q_ASSERT(!memory_dc_);

        // Vote to disable Aero composited desktop effects while capturing.
        // Windows will restore Aero automatically if the process exits.
        // This has no effect under Windows 8 or higher. See crbug.com/124018.
        DwmEnableComposition(DWM_EC_DISABLECOMPOSITION);

        // Create GDI device contexts to capture from the desktop into memory.
        desktop_dc_ = std::make_unique<ScopedGetDC>(nullptr);
        memory_dc_.reset(CreateCompatibleDC(*desktop_dc_));
        if (!memory_dc_)
        {
            qWarning("CreateCompatibleDC failed");
            return false;
        }

        desktop_dc_rect_ = screen_rect;

        for (int i = 0; i < kNumFrames; ++i)
        {
            frame_[i] = DesktopFrameDIB::create(screen_rect.size(),
                                                PixelFormat::ARGB(),
                                                memory_dc_);
            if (!frame_[i])
                return false;
        }

        differ_ = std::make_unique<Differ>(screen_rect.size());
    }

    return true;
}

const DesktopFrame* ScreenCapturerGDI::captureImage()
{
    if (!prepareCaptureResources())
        return nullptr;

    int prev_frame_id = curr_frame_id_ - 1;
    if (prev_frame_id < 0)
        prev_frame_id = kNumFrames - 1;

    DesktopFrameDIB* prev_frame = frame_[prev_frame_id].get();
    DesktopFrameDIB* curr_frame = frame_[curr_frame_id_].get();

    HGDIOBJ old_bitmap = SelectObject(memory_dc_, curr_frame->bitmap());
    if (old_bitmap)
    {
        BitBlt(memory_dc_,
               0, 0,
               curr_frame->size().width(),
               curr_frame->size().height(),
               *desktop_dc_,
               desktop_dc_rect_.x(),
               desktop_dc_rect_.y(),
               CAPTUREBLT | SRCCOPY);

        SelectObject(memory_dc_, old_bitmap);
    }

    differ_->calcDirtyRegion(prev_frame->frameData(),
                             curr_frame->frameData(),
                             curr_frame->mutableUpdatedRegion());

    curr_frame_id_ = prev_frame_id;

    return curr_frame;
}

} // namespace aspia
