#include "pch.h"
#include "App.h"

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    (void)hPrevInstance;

    try {
        // Parse command line for file path
        std::wstring initialFile;
        if (lpCmdLine && lpCmdLine[0] != L'\0') {
            initialFile = lpCmdLine;

            // Remove quotes if present
            if (!initialFile.empty() && initialFile.front() == L'"' && initialFile.back() == L'"') {
                initialFile = initialFile.substr(1, initialFile.length() - 2);
            }
        }

        App app;
        if (!app.Initialize(hInstance, nCmdShow, initialFile)) {
            MessageBoxW(nullptr, L"Failed to initialize application", L"fotos", MB_ICONERROR);
            return 1;
        }

        return app.Run();
    }
    catch (const std::exception& e) {
        std::wstring msg = L"Exception: ";
        msg += std::wstring(e.what(), e.what() + strlen(e.what()));
        MessageBoxW(nullptr, msg.c_str(), L"fotos Error", MB_ICONERROR);
        return 1;
    }
    catch (...) {
        MessageBoxW(nullptr, L"Unknown exception occurred", L"fotos Error", MB_ICONERROR);
        return 1;
    }
}
