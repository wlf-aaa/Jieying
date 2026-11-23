// C++/WinRT and the interop headers first, so Qt's keyword macros cannot leak into them.
#include <unknwn.h>
#include <inspectable.h>
#include <roapi.h>
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

#include <atomic>
#include <cstring>

using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

namespace {

constexpr int kFrameCount = 2;
constexpr auto kPixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;

struct CaptureResources
{
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<ID3D11Texture2D> staging;
    IDirect3DDevice winrtDevice{nullptr};
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};

    ~CaptureResources()
    {
        if (session) {
            try {
                session.Close();
            } catch (const winrt::hresult_error &) {
            }
        }
        if (pool) {
            try {
                pool.Close();
            } catch (const winrt::hresult_error &) {
            }
        }
    }

    void createDevice()
    {
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                               device.put(), nullptr, context.put()));
        const auto dxgiDevice = device.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
        winrtDevice = inspectable.as<IDirect3DDevice>();
    }

    void createSession(winrt::Windows::Graphics::SizeInt32 size, bool includeCursor)
    {
        // The free-threaded pool allows polling without a Windows DispatcherQueue.
        pool = Direct3D11CaptureFramePool::CreateFreeThreaded(winrtDevice, kPixelFormat, kFrameCount, size);
        session = pool.CreateCaptureSession(item);
        try {
            session.IsCursorCaptureEnabled(includeCursor); // Windows 10 2004+
        } catch (const winrt::hresult_error &) {
        }
    }

    void createStagingTexture(const QSize &size)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(size.width());
        desc.Height = UINT(size.height());
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging = nullptr;
        winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, staging.put()));
    }

    void copyFrame(const Direct3D11CaptureFrame &frame, const QRect &region)
    {
        const auto access =
            frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(winrt::guid_of<ID3D11Texture2D>(), texture.put_void()));
        const D3D11_BOX box{UINT(region.left()), UINT(region.top()), 0, UINT(region.right() + 1),
                            UINT(region.bottom() + 1), 1};
        context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture.get(), 0, &box);
    }

    QImage readStagingTexture(const QSize &size, UINT mapFlags = 0)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = context->Map(staging.get(), 0, D3D11_MAP_READ, mapFlags, &mapped);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING && mapFlags == D3D11_MAP_FLAG_DO_NOT_WAIT)
            return {};
        winrt::check_hresult(result);
        QImage image(size, QImage::Format_RGB32);
        const auto *source = static_cast<const uchar *>(mapped.pData);
        for (int y = 0; y < image.height(); ++y) {
            auto *row = reinterpret_cast<quint32 *>(image.scanLine(y));
            std::memcpy(row, source + size_t(y) * mapped.RowPitch, size_t(image.width()) * sizeof(*row));
            for (int x = 0; x < image.width(); ++x)
                row[x] |= 0xFF000000u; // RGB32 expects an opaque alpha byte
        }
        context->Unmap(staging.get(), 0);
        if (image.isNull())
            winrt::throw_hresult(E_OUTOFMEMORY);
        return image;
    }
};

class CaptureApartment
{
public:
    CaptureApartment()
        : m_result(RoInitialize(RO_INIT_SINGLETHREADED))
    {
        // A thread already initialized as MTA can use the same free-threaded capture objects.
        if (m_result != RPC_E_CHANGED_MODE)
            winrt::check_hresult(m_result);
    }

    ~CaptureApartment()
    {
        if (SUCCEEDED(m_result))
            RoUninitialize();
    }

private:
    HRESULT m_result;
};

QString captureError(const winrt::hresult_error &error)
{
    const QString code = QStringLiteral("0x%1").arg(quint32(error.code()), 8, 16, QLatin1Char('0'));
    const QString message = QString::fromWCharArray(error.message().c_str()).trimmed();
    return message.isEmpty() ? code : QStringLiteral("%1 (%2)").arg(message, code);
}

} // namespace

struct WgcGrabber::Private : CaptureResources
{
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

        p->createDevice();

        const auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<GraphicsCaptureItem>(),
                                                       winrt::put_abi(p->item)));

        p->createSession(p->item.Size(), includeCursor);
        if (canHideBorder()) {
            try {
                p->session.IsBorderRequired(false); // Windows 11
            } catch (const winrt::hresult_error &) {
            }
        }

        p->createStagingTexture(p->region.size());

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

        d->copyFrame(frame, d->region);
        d->last = d->readStagingTexture(d->region.size());
    } catch (const winrt::hresult_error &e) {
        m_error = QString::fromWCharArray(e.message().c_str());
    }
    return d->last;
}

void WgcGrabber::stop()
{
    d.reset();
}

struct WgcWindowGrabber::Private
{
    CaptureApartment apartment;
    CaptureResources capture;
    std::shared_ptr<std::atomic_bool> closed = std::make_shared<std::atomic_bool>(false);
    GraphicsCaptureItem::Closed_revoker closedEvent;
    QSize frameSize;
    bool readbackPending = false;
};

WgcWindowGrabber::WgcWindowGrabber() = default;
WgcWindowGrabber::~WgcWindowGrabber() = default;

bool WgcWindowGrabber::isSupported()
{
    try {
        const CaptureApartment apartment;
        return WgcGrabber::isSupported()
            && bool(winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>());
    } catch (const winrt::hresult_error &) {
        return false;
    }
}

bool WgcWindowGrabber::start(quintptr window)
{
    stop();
    m_error.clear();
    const char *operation = "Windows Graphics Capture";
    try {
        auto p = std::make_unique<Private>();
        auto &capture = p->capture;
        const auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        operation = "CreateForWindow";
        winrt::check_hresult(interop->CreateForWindow(reinterpret_cast<HWND>(window),
            winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(capture.item)));
        p->closedEvent = capture.item.Closed(winrt::auto_revoke, [closed = p->closed](const auto &, const auto &) {
            closed->store(true, std::memory_order_relaxed);
        });
        const auto size = capture.item.Size();
        p->frameSize = {size.Width, size.Height};
        if (p->frameSize.isEmpty()) {
            m_error = QStringLiteral("The selected window has no capturable area");
            return false;
        }
        operation = "D3D11CreateDevice";
        capture.createDevice();
        operation = "CreateCaptureSession";
        capture.createSession(size, false);
        operation = "CreateTexture2D";
        capture.createStagingTexture(p->frameSize);
        operation = "StartCapture";
        capture.session.StartCapture();
        d = std::move(p);
        return true;
    } catch (const winrt::hresult_error &e) {
        m_error = QStringLiteral("%1 · %2").arg(QString::fromLatin1(operation), captureError(e));
        return false;
    }
}

WindowCaptureFrame WgcWindowGrabber::poll()
{
    using Status = WindowCaptureFrame::Status;
    if (!d)
        return {};
    if (d->closed->load(std::memory_order_relaxed))
        return {Status::Closed, {}};

    try {
        auto &capture = d->capture;
        QImage image;
        if (d->readbackPending) {
            image = capture.readStagingTexture(d->frameSize, D3D11_MAP_FLAG_DO_NOT_WAIT);
            if (image.isNull())
                return {};
            d->readbackPending = false;
        }

        auto frame = capture.pool.TryGetNextFrame();
        if (!frame)
            return image.isNull() ? WindowCaptureFrame{} : WindowCaptureFrame{Status::Frame, image};
        // The pool is bounded. Prefer the latest frame without looping while the compositor produces more.
        if (auto newer = capture.pool.TryGetNextFrame())
            frame = std::move(newer);
        const auto size = frame.ContentSize();
        const QSize content(size.Width, size.Height);
        if (content.isEmpty())
            return {};
        if (content != d->frameSize) {
            // The old pool cannot hold the new frame; drop it before recreating the buffers.
            frame.Close();
            frame = nullptr;
            capture.pool.Recreate(capture.winrtDevice, kPixelFormat, kFrameCount, size);
            capture.createStagingTexture(content);
            d->frameSize = content;
            return {Status::Resizing, {}};
        }
        capture.copyFrame(frame, QRect(QPoint(0, 0), d->frameSize));
        capture.context->Flush();
        d->readbackPending = true;
        return image.isNull() ? WindowCaptureFrame{} : WindowCaptureFrame{Status::Frame, image};
    } catch (const winrt::hresult_error &e) {
        if (d->closed->load(std::memory_order_relaxed))
            return {Status::Closed, {}};
        m_error = captureError(e);
        return {Status::Error, {}};
    }
}

void WgcWindowGrabber::stop()
{
    d.reset();
}
