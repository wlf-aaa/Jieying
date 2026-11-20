// C++/WinRT and the interop headers first, so Qt's keyword macros cannot leak into them.
#include <unknwn.h>
#include <inspectable.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <dxgi.h>

#include "wgcgrabber.h"

#include <QElapsedTimer>
#include <QThread>

#include <cstring>

using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

struct WgcGrabber::Private
{
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<ID3D11Texture2D> staging;
    IDirect3DDevice winrtDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    QRect region; // relative to the monitor's top-left, physical pixels
    QImage last;
};

WgcGrabber::WgcGrabber() = default;

WgcGrabber::~WgcGrabber()
{
    stop();
}

bool WgcGrabber::isSupported()
{
    try {
        return GraphicsCaptureSession::IsSupported();
    } catch (const winrt::hresult_error &) {
        return false;
    }
}

bool WgcGrabber::canHideBorder()
{
    try {
        return ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired");
    } catch (const winrt::hresult_error &) {
        return false;
    }
}

bool WgcGrabber::start(const QRect &physicalRect, bool includeCursor)
{
    stop();
    if (!isSupported()) {
        m_error = QStringLiteral("Windows.Graphics.Capture is not supported here");
        return false;
    }
    try {
        auto p = std::make_unique<Private>();

        const POINT center{physicalRect.center().x(), physicalRect.center().y()};
        HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(monitor, &info);
        const QRect monitorRect(QPoint(info.rcMonitor.left, info.rcMonitor.top),
                                QPoint(info.rcMonitor.right - 1, info.rcMonitor.bottom - 1));
        p->region = physicalRect.intersected(monitorRect).translated(-monitorRect.topLeft());
        if (p->region.isEmpty()) {
            m_error = QStringLiteral("region is outside the monitor");
            return false;
        }

        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                               p->device.put(), nullptr, p->context.put()));
        const auto dxgiDevice = p->device.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
        p->winrtDevice = inspectable.as<IDirect3DDevice>();

        const auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<GraphicsCaptureItem>(),
                                                       winrt::put_abi(p->item)));

        // Free-threaded pool: frames can be fetched from whatever thread calls grab().
        p->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            p->winrtDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, p->item.Size());
        p->session = p->pool.CreateCaptureSession(p->item);
        try {
            p->session.IsCursorCaptureEnabled(includeCursor); // Windows 10 2004+
        } catch (const winrt::hresult_error &) {
        }
        if (canHideBorder()) {
            try {
                p->session.IsBorderRequired(false); // Windows 11
            } catch (const winrt::hresult_error &) {
            }
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(p->region.width());
        desc.Height = UINT(p->region.height());
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        winrt::check_hresult(p->device->CreateTexture2D(&desc, nullptr, p->staging.put()));

        p->session.StartCapture();
        d = std::move(p);
        return true;
    } catch (const winrt::hresult_error &e) {
        m_error = QString::fromWCharArray(e.message().c_str());
        return false;
    }
}

QImage WgcGrabber::grab()
{
    if (!d)
        return {};
    try {
        // The first frame arrives shortly after StartCapture(); later, keep only the newest.
        Direct3D11CaptureFrame frame = d->pool.TryGetNextFrame();
        if (!frame && d->last.isNull()) {
            QElapsedTimer waited;
            waited.start();
            while (!frame && waited.elapsed() < 1500) {
                QThread::msleep(5);
                frame = d->pool.TryGetNextFrame();
            }
        }
        for (Direct3D11CaptureFrame next = frame ? d->pool.TryGetNextFrame() : nullptr; next;
             next = d->pool.TryGetNextFrame())
            frame = next;
        if (!frame)
            return d->last;

        const auto access =
            frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void()));
        const D3D11_BOX box{UINT(d->region.left()), UINT(d->region.top()), 0, UINT(d->region.right() + 1),
                            UINT(d->region.bottom() + 1), 1};
        d->context->CopySubresourceRegion(d->staging.get(), 0, 0, 0, 0, texture.get(), 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(d->context->Map(d->staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        QImage image(d->region.size(), QImage::Format_RGB32);
        const auto *source = static_cast<const uchar *>(mapped.pData);
        for (int y = 0; y < image.height(); ++y) {
            auto *row = reinterpret_cast<quint32 *>(image.scanLine(y));
            std::memcpy(row, source + size_t(y) * mapped.RowPitch, size_t(image.width()) * 4);
            for (int x = 0; x < image.width(); ++x)
                row[x] |= 0xFF000000u; // RGB32 expects an opaque alpha byte
        }
        d->context->Unmap(d->staging.get(), 0);
        d->last = image;
    } catch (const winrt::hresult_error &e) {
        m_error = QString::fromWCharArray(e.message().c_str());
    }
    return d->last;
}

void WgcGrabber::stop()
{
    if (!d)
        return;
    try {
        d->session.Close();
        d->pool.Close();
    } catch (const winrt::hresult_error &) {
    }
    d.reset();
}
