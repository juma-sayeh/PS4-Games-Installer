// Exercise the real native Video keyboard path with the pinned SDK's ABI.
#include "video.hpp"
#include <orbis/CommonDialog.h>
#include <orbis/ImeDialog.h>
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
OrbisImeDialogSetting captured{};
bool dialog = false, finished = false;
OrbisDialogResultStatus completion = ORBIS_DIALOG_CANCEL;
int lastValidation = 0, forcedError = 0;
constexpr uint32_t Password = 4;

int validateDialog(const OrbisImeDialogSetting& params) {
    // Public dialog constraints for the SDK version declared by the pinned
    // OpenOrbis crt1 (0x01000051), reproduced by shadPS4's validation:
    // https://github.com/shadps4-emu/shadPS4/blob/443676d825d3b5c3f7d7fe3c84899b5896892c2d/src/core/libraries/ime/ime_dialog.cpp
    if (params.option & ~uint32_t(0x080002ff)) return int32_t(0x80bc0015u);
    if ((params.option & Password) && params.type != ORBIS_TYPE_BASIC_LATIN && params.type != ORBIS_TYPE_NUMBER)
        return int32_t(0x80bc0030u);
    if (!params.maxTextLength || params.maxTextLength > 2048) return int32_t(0x80bc0016u);
    if (!params.inputTextBuffer || !params.title) return int32_t(0x80bc0017u);
    return 0;
}
}

extern "C" {
uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal) { return 0; }
int32_t sceSysmoduleLoadModule(OrbisSysModule module) { assert(module == ORBIS_SYSMODULE_IME_DIALOG); return 0; }
int32_t sceUserServiceInitialize(void*) { return 0; }
int32_t sceUserServiceGetInitialUser(int32_t* id) { *id = 7; return 0; }
int32_t scePadInit() { return 0; }
int32_t scePadOpen(int32_t user, int32_t, int32_t, void*) { assert(user == 7); return 8; }
int32_t scePadClose(int32_t id) { assert(id == 8); return 0; }
int32_t scePadReadState(int32_t id, OrbisPadData* data) {
    assert(id == 8); *data = {}; data->connected = 1; data->leftStick = {128, 128}; return 0;
}
int32_t sceVideoOutOpen(OrbisUserServiceUserId, int32_t, int32_t, const void*) { return 9; }
int32_t sceVideoOutClose(int32_t id) { assert(id == 9); return 0; }
void sceVideoOutSetBufferAttribute(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
int32_t sceVideoOutRegisterBuffers(int32_t, int32_t, void* const*, int32_t, const OrbisVideoOutBufferAttribute*) { return 0; }
int32_t sceVideoOutSetFlipRate(int32_t, int32_t) { return 0; }
size_t sceKernelGetDirectMemorySize() { return 64 * 1024 * 1024; }
int32_t sceKernelAllocateDirectMemory(off_t, off_t, size_t, size_t, int32_t, off_t* offset) { *offset = 0; return 0; }
int32_t sceKernelMapDirectMemory(void** output, size_t size, int32_t, int32_t, off_t, size_t) {
    *output = std::calloc(1, size); return *output ? 0 : -1;
}
int32_t sceKernelMunmap(void* memory, size_t) { std::free(memory); return 0; }
int32_t sceKernelReleaseDirectMemory(off_t, size_t) { return 0; }
int32_t sceCommonDialogInitialize() { return 0; }
int32_t sceImeDialogInit(const OrbisImeDialogSetting* params, OrbisImeSettingsExtended* extended) {
    assert(params && !extended && !dialog); captured = *params;
    lastValidation = forcedError ? forcedError : validateDialog(*params);
    if (lastValidation) return lastValidation;
    dialog = true; finished = false; return 0;
}
void sceImeDialogAbort() { assert(dialog); finished = true; }
int32_t sceImeDialogTerm() { assert(dialog); dialog = finished = false; return 0; }
OrbisDialogStatus sceImeDialogGetStatus() { return finished ? ORBIS_DIALOG_STATUS_STOPPED : ORBIS_DIALOG_STATUS_RUNNING; }
int32_t sceImeDialogGetResult(OrbisDialogResult* result) {
    assert(dialog && finished); *result = {}; result->endstatus = completion; return 0;
}
}

int main() {
    using namespace psgi;
    Video video;
    Ui ui;
    for (auto kind : {EditKind::ArchiveEmail, EditKind::SourceUrl, EditKind::ArchivePassword}) {
        ui.editKind = kind; ui.screen = Screen::Keyboard; ui.keyboardText = "temporary";
        ui.keyboardReturn = kind == EditKind::SourceUrl ? Screen::Settings : Screen::ArchiveAccount;
        try { video.startKeyboard(ui); }
        catch (const std::exception& error) {
            // Report only API parameters and error code, never entered text.
            std::fprintf(stderr, "Native IME rejected kind=%u type=%u options=0x%X max=%u code=0x%08X: %s\n",
                         unsigned(kind), unsigned(captured.type), captured.option, captured.maxTextLength,
                         unsigned(lastValidation), error.what());
            return 1;
        }
        assert(captured.userId == 7 && captured.maxTextLength == keyboardMaximum(kind));
        if (kind == EditKind::ArchivePassword) {
            assert(captured.type == ORBIS_TYPE_BASIC_LATIN);
            assert(captured.option & Password);
            assert(captured.option & 32); // Password input must not train keyboard suggestions.
            assert(!(captured.option & 256)); // Unsupported by our declared SDK version.
            assert(captured.maxTextLength == 1024);
        } else {
            assert(captured.type == (kind == EditKind::ArchiveEmail ? ORBIS_TYPE_MAIL : ORBIS_TYPE_TYPE_URL));
            assert(captured.option == 0);
        }
        completion = ORBIS_DIALOG_CANCEL; finished = true; video.pollKeyboard(ui);
        const auto* buffer = reinterpret_cast<const char16_t*>(captured.inputTextBuffer);
        assert(std::all_of(buffer, buffer + captured.maxTextLength + 1, [](char16_t c) { return c == 0; }));
        assert(!dialog && ui.archivePassword.empty());
    }

    ui.editKind = EditKind::ArchivePassword; ui.screen = Screen::Keyboard;
    ui.keyboardReturn = Screen::ArchiveAccount; ui.keyboardText.clear();
    video.startKeyboard(ui);
    const std::u16string entered = u"fixture-password";
    auto* buffer = reinterpret_cast<char16_t*>(captured.inputTextBuffer);
    std::copy(entered.begin(), entered.end(), buffer);
    completion = ORBIS_DIALOG_OK; finished = true; video.pollKeyboard(ui);
    assert(ui.screen == Screen::ArchiveAccount && ui.archivePassword == "fixture-password" && ui.keyboardText.empty());
    assert(std::all_of(buffer, buffer + captured.maxTextLength + 1, [](char16_t c) { return c == 0; }));

    forcedError = int32_t(0x80bc0030u); ui.keyboardText = "sensitive-fixture";
    try { video.startKeyboard(ui); assert(false); }
    catch (const std::runtime_error& error) {
        assert(std::string(error.what()).find("sensitive-fixture") == std::string::npos);
        assert(std::string(error.what()).find("0x80BC0030") != std::string::npos);
        buffer = reinterpret_cast<char16_t*>(captured.inputTextBuffer);
        assert(std::all_of(buffer, buffer + captured.maxTextLength + 1, [](char16_t c) { return c == 0; }));
    }
    assert(!dialog);
    std::puts("Native IME password type, supported options, email/URL modes and buffer cleanup passed");
}
