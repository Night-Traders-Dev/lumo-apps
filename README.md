# Lumo OS Native Applications

This repository contains the suite of 14 native, touch-first Wayland applications for **Lumo OS**, built to run on the SpacemiT K1 / OrangePi RV2 RISC-V SoC.

These applications are pulled into the main [Lumo-Compositor](https://github.com/Night-Traders-Dev/Lumo-Compositor) repository as a Git submodule under `compositor/src/apps`.

## App Suite

Each tile in the Lumo shell drawer corresponds to a lightweight, double-buffered SHM client in this repository:

* **Phone**: Mock dialing interface with tabbed layout.
* **Terminal**: `/bin/sh` shell wrapper using a PTY backend.
* **Browser**: WebKitGTK-based Wayland client.
* **Camera**: Viewfinder stub with gallery view support.
* **Maps**: Standard mock map client with coordinate reading and places list.
* **Music**: Audio track library with `mpv` rendering integration.
* **Photos**: 3-column image gallery supporting JPEG and PNG files.
* **Videos**: Video player library with thumbnail view.
* **Clock**: Multi-tab stopwatch, alarm system, and countdown timer.
* **Notes**: Interactive editor with blinking cursor and OSK input.
* **Files**: Directory browser with navigation and details.
* **Settings**: 8-tab settings dashboard (battery, display, networking, etc.).

## Architecture & Rendering

* **Pixman/SHM Backend**: Since the target RISC-V SoC lacks hardware GPU drivers, apps use double-buffered shared memory (SHM) buffers for fast software rendering.
* **Shared Rendering API**: Drawing utilities are provided in `app_render.h` (fill, gradients, rounded rectangles, text layouts).
* **Double-Buffered Events**: Synchronized Wayland frame events to prevent tearing during screen resizing and rotation transforms.
* **OSK Integration**: Communicates text state via the `text-input-v3` Wayland protocol to trigger the compositor's on-screen keyboard (OSK).

## Contribution

For core changes, modify files here and commit them to this repository, then update the submodule reference inside the main compositor repository.
