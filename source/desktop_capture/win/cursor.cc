//
// PROJECT:         Aspia
// FILE:            desktop_capture/win/cursor.cc
// LICENSE:         GNU General Public License 3
// PROGRAMMERS:     Dmitry Chapyshev (dmitry@aspia.ru)
//

#include "desktop_capture/win/cursor.h"

#include "base/win/scoped_gdi_object.h"
#include "desktop_capture/mouse_cursor.h"

namespace aspia {

namespace {

constexpr quint32 RGBA(quint32 r, quint32 g, quint32 b, quint32 a)
{
    return (((a << 24) & 0xFF000000) | ((b << 16) & 0xFF0000) | ((g << 8) & 0xFF00) | (r & 0xFF));
}

constexpr int kBytesPerPixel = 4;

// Pixel colors used when generating cursor outlines.
constexpr quint32 kPixelRgbaBlack       = RGBA(0,    0,    0,    0xFF);
constexpr quint32 kPixelRgbaWhite       = RGBA(0xFF, 0xFF, 0xFF, 0xFF);
constexpr quint32 kPixelRgbaTransparent = RGBA(0,    0,    0,    0);

constexpr quint32 kPixelRgbWhite = RGB(0xFF, 0xFF, 0xFF);

// Scans a 32bpp bitmap looking for any pixels with non-zero alpha component.
// Returns true if non-zero alpha is found. |stride| is expressed in pixels.
bool hasAlphaChannel(const quint32* data, int width, int height)
{
    const RGBQUAD* plane = reinterpret_cast<const RGBQUAD*>(data);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            if (plane->rgbReserved != 0)
                return true;

            ++plane;
        }
    }

    return false;
}

// Expands the cursor shape to add a white outline for visibility against
// dark backgrounds.
void addCursorOutline(int width, int height, quint32* data)
{
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            // If this is a transparent pixel (bgr == 0 and alpha = 0), check
            // the neighbor pixels to see if this should be changed to an
            // outline pixel.
            if (*data == kPixelRgbaTransparent)
            {
                // Change to white pixel if any neighbors (top, bottom, left,
                // right) are black.
                if ((y > 0 && data[-width] == kPixelRgbaBlack) ||
                    (y < height - 1 && data[width] == kPixelRgbaBlack) ||
                    (x > 0 && data[-1] == kPixelRgbaBlack) ||
                    (x < width - 1 && data[1] == kPixelRgbaBlack))
                {
                    *data = kPixelRgbaWhite;
                }
            }

            ++data;
        }
    }
}

// Premultiplies RGB components of the pixel data in the given image by
// the corresponding alpha components.
void alphaMul(quint32* data, int width, int height)
{
    static_assert(sizeof(quint32) == kBytesPerPixel,
                  "size of uint32 should be the number of bytes per pixel");

    for (quint32* data_end = data + width * height; data != data_end; ++data)
    {
        RGBQUAD* from = reinterpret_cast<RGBQUAD*>(data);
        RGBQUAD* to = reinterpret_cast<RGBQUAD*>(data);

        to->rgbBlue  = (static_cast<quint16>(from->rgbBlue)  * from->rgbReserved) / 0xFF;
        to->rgbGreen = (static_cast<quint16>(from->rgbGreen) * from->rgbReserved) / 0xFF;
        to->rgbRed   = (static_cast<quint16>(from->rgbRed)   * from->rgbReserved) / 0xFF;
    }
}

} // namespace

// Converts an HCURSOR into a |MouseCursor| instance.
MouseCursor* mouseCursorFromHCursor(HDC dc, HCURSOR cursor)
{
    ICONINFO icon_info = { 0 };

    if (!GetIconInfo(cursor, &icon_info))
    {
        qWarning("GetIconInfo failed");
        return nullptr;
    }

    // Make sure the bitmaps will be freed.
    ScopedHBITMAP scoped_mask(icon_info.hbmMask);
    ScopedHBITMAP scoped_color(icon_info.hbmColor);

    bool is_color = (icon_info.hbmColor != nullptr);

    // Get |scoped_mask| dimensions.
    BITMAP bitmap_info;

    if (!GetObjectW(scoped_mask, sizeof(bitmap_info), &bitmap_info))
    {
        qWarning("GetObjectW failed");
        return nullptr;
    }

    int width = bitmap_info.bmWidth;
    int height = bitmap_info.bmHeight;

    std::unique_ptr<quint32[]> mask_data = std::make_unique<quint32[]>(width * height);

    // Get pixel data from |scoped_mask| converting it to 32bpp along the way.
    // GetDIBits() sets the alpha component of every pixel to 0.
    BITMAPV5HEADER bmi = { 0 };

    bmi.bV5Size        = sizeof(bmi);
    bmi.bV5Width       = width;
    bmi.bV5Height      = -height; // request a top-down bitmap.
    bmi.bV5Planes      = 1;
    bmi.bV5BitCount    = kBytesPerPixel * 8;
    bmi.bV5Compression = BI_RGB;
    bmi.bV5AlphaMask   = 0xFF000000;
    bmi.bV5CSType      = LCS_WINDOWS_COLOR_SPACE;
    bmi.bV5Intent      = LCS_GM_BUSINESS;

    if (!GetDIBits(dc,
                   scoped_mask,
                   0,
                   height,
                   mask_data.get(),
                   reinterpret_cast<BITMAPINFO*>(&bmi),
                   DIB_RGB_COLORS))
    {
        qWarning("GetDIBits failed");
        return nullptr;
    }

    uint32_t* mask_plane = mask_data.get();

    std::unique_ptr<quint8[]> image;
    size_t image_size;

    bool has_alpha = false;

    if (is_color)
    {
        image_size = width * height * kBytesPerPixel;
        image = std::make_unique<quint8[]>(image_size);

        // Get the pixels from the color bitmap.
        if (!GetDIBits(dc,
                       scoped_color,
                       0,
                       height,
                       image.get(),
                       reinterpret_cast<BITMAPINFO*>(&bmi),
                       DIB_RGB_COLORS))
        {
            qWarning("GetDIBits failed");
            return nullptr;
        }

        // GetDIBits() does not provide any indication whether the bitmap has
        // alpha channel, so we use HasAlphaChannel() below to find it out.
        has_alpha = hasAlphaChannel(reinterpret_cast<const quint32*>(image.get()), width, height);
    }
    else
    {
        // For non-color cursors, the mask contains both an AND and an XOR mask
        // and the height includes both. Thus, the width is correct, but we
        // need to divide by 2 to get the correct mask height.
        height /= 2;

        image_size = width * height * kBytesPerPixel;
        image = std::make_unique<quint8[]>(image_size);

        // The XOR mask becomes the color bitmap.
        memcpy(image.get(), mask_plane + (width * height), image_size);
    }

    //
    // Reconstruct transparency from the mask if the color image does not has
    // alpha channel.
    //
    if (!has_alpha)
    {
        bool add_outline = false;
        quint32* dst = reinterpret_cast<quint32*>(image.get());
        quint32* mask = mask_plane;

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                // The two bitmaps combine as follows:
                //  mask  color   Windows Result   Our result    RGB   Alpha
                //   0     00      Black            Black         00    ff
                //   0     ff      White            White         ff    ff
                //   1     00      Screen           Transparent   00    00
                //   1     ff      Reverse-screen   Black         00    ff
                //
                // Since we don't support XOR cursors, we replace the "Reverse
                // Screen" with black. In this case, we also add an outline
                // around the cursor so that it is visible against a dark
                // background.
                if (*mask == kPixelRgbWhite)
                {
                    if (*dst != 0)
                    {
                        add_outline = true;
                        *dst = kPixelRgbaBlack;
                    }
                    else
                    {
                        *dst = kPixelRgbaTransparent;
                    }
                }
                else
                {
                    *dst = kPixelRgbaBlack ^ *dst;
                }

                ++dst;
                ++mask;
            }
        }

        if (add_outline)
        {
            addCursorOutline(width, height, reinterpret_cast<quint32*>(image.get()));
        }
    }

    // Pre-multiply the resulting pixels since MouseCursor uses premultiplied
    // images.
    alphaMul(reinterpret_cast<quint32*>(image.get()), width, height);

    return new MouseCursor(std::move(image),
                           QSize(width, height),
                           QPoint(icon_info.xHotspot, icon_info.yHotspot));
}

} // namespace aspia
