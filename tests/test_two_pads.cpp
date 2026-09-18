// O23: does a second pad really reach only the second player?
//
// The R36S has one built-in controller, so a second player needs a second pad plugged in - and the author has no
// second pad to try it with. This test supplies one: SDL can attach VIRTUAL joysticks (SDL_JoystickAttachVirtual,
// 2.0.14+), so two pads are created here, opened through the game's own Input::init, and pressed one at a time.
//
// What it proves and what it does not:
//   * PROVES that Input tells two SDL devices apart by the instance id carried on every event, that each ends up
//     on its own device index, and that pressing one never shows up on the other. That is the whole of the
//     per-pad logic, and it is the same source file the R36S build compiles.
//   * DOES NOT prove anything about the device's own SDL (2.0.9) or about how ArkOS enumerates the built-in
//     controls. Virtual joysticks do not exist in 2.0.9 - which is why this test is PC only. Every SDL call the
//     game itself makes (SDL_JoystickInstanceID, SDL_GameControllerGetJoystick, the `which` field of the joystick
//     and controller events) has been in SDL since 2.0.0 and is present in the device's headers.
//
//   test_two_pads.exe
#include <cstdio>
#include <vector>

#include <SDL.h>

#include "engine/input.h"

using namespace cr;

static int failures = 0;

static void check(bool ok, const char *what)
{
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

// hand SDL's queue to Input the way Platform::events() does
static void pump(Input &in)
{
    SDL_JoystickUpdate();
    std::vector<SDL_Event> events;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) events.push_back(ev);
    in.handleEvents(events);
    in.step();
}

int main()
{
    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        std::printf("test_two_pads: SDL_Init failed: %s\n", SDL_GetError());
        return 2;
    }
    // two pads, as if somebody had plugged a second one into the console
    const int slotA = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 2, 8, 1);
    const int slotB = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 2, 8, 1);
    if (slotA < 0 || slotB < 0) {
        std::printf("test_two_pads: cannot attach virtual joysticks: %s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }
    SDL_Joystick *a = SDL_JoystickOpen(slotA), *b = SDL_JoystickOpen(slotB);
    if (!a || !b) {
        std::printf("test_two_pads: cannot open the virtual joysticks\n");
        SDL_Quit();
        return 2;
    }

    Input in;
    in.logRawButtons = false;
    in.init(); // opens everything SDL reports, exactly as the game does
    check(in.padCount() == 2, "the game found two pads");

    pump(in); // settle

    // --- pad one alone
    SDL_JoystickSetVirtualButton(a, 0, SDL_PRESSED); // button 0 is A in the raw-joystick fallback
    pump(in);
    check(in.deviceDown(3, ActA), "pad 1 pressed: player one's device sees it");
    check(!in.deviceDown(4, ActA), "pad 1 pressed: player TWO's device does NOT");
    check(in.down(ActA), "pad 1 pressed: device 0 (the menus) sees it");
    SDL_JoystickSetVirtualButton(a, 0, SDL_RELEASED);
    pump(in);
    check(!in.deviceDown(3, ActA), "pad 1 released: player one's device is clear");

    // --- pad two alone
    SDL_JoystickSetVirtualButton(b, 0, SDL_PRESSED);
    pump(in);
    check(in.deviceDown(4, ActA), "pad 2 pressed: player two's device sees it");
    check(!in.deviceDown(3, ActA), "pad 2 pressed: player ONE's device does NOT");
    SDL_JoystickSetVirtualButton(b, 0, SDL_RELEASED);
    pump(in);
    check(!in.deviceDown(4, ActA), "pad 2 released: player two's device is clear");

    // --- both at once, different buttons: neither leaks into the other
    SDL_JoystickSetVirtualButton(a, 0, SDL_PRESSED); // A
    SDL_JoystickSetVirtualButton(b, 1, SDL_PRESSED); // B in the fallback table
    pump(in);
    check(in.deviceDown(3, ActA) && !in.deviceDown(3, ActB), "both pressed: player one has only its own button");
    check(in.deviceDown(4, ActB) && !in.deviceDown(4, ActA), "both pressed: player two has only its own button");

    // --- the stick. SDL maps a virtual pad's axes to the controller's left stick, which is the path a real pad's
    // directions take once ArkOS has given it a mapping. (The raw-hat path in Input only runs for a pad with NO
    // mapping at all, and SDL's own virtual mapping carries no hat, so it cannot be exercised from here.)
    SDL_JoystickSetVirtualButton(a, 0, SDL_RELEASED);
    SDL_JoystickSetVirtualButton(b, 1, SDL_RELEASED);
    pump(in);
    SDL_JoystickSetVirtualAxis(b, 1, -32000); // left stick up
    pump(in);
    check(in.deviceDown(4, ActUp), "pad 2 stick up: player two's device sees it");
    check(!in.deviceDown(3, ActUp), "pad 2 stick up: player ONE's device does NOT");
    SDL_JoystickSetVirtualAxis(a, 0, 32000); // pad one's stick right, at the same time
    pump(in);
    check(in.deviceDown(3, ActRight) && !in.deviceDown(3, ActUp), "both sticks: player one has only its own");
    check(in.deviceDown(4, ActUp) && !in.deviceDown(4, ActRight), "both sticks: player two has only its own");

    SDL_JoystickClose(a);
    SDL_JoystickClose(b);
    SDL_JoystickDetachVirtual(slotA);
    SDL_JoystickDetachVirtual(slotB);
    in.shutdown();
    SDL_Quit();

    if (failures) {
        std::printf("test_two_pads: %d FAILED\n", failures);
        return 1;
    }
    std::printf("test_two_pads: OK\n");
    return 0;
}
