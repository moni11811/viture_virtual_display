# V4L2 Viture Virtual Display

**Attention** this is still in development and not fully functional yet 

v4l2_gl captures video from a HDMI-in on an OrangePI 5 Plus using the hdmirx V4L2 device, converts frames to RGB, and displays them in real-time on a textured quad in an OpenGL window. It supports Viture headset IMU integration, test patterns, and plane geometry.

![Diagram of virtual display](https://github.com/mgschwan/viture_virtual_display/blob/main/assets/virtual_display.png?raw=true)
## Prerequisites

To run this as intended you need an OrangePi 5 Plus with armbian ( we are using BredOS ) that has Device Tree Overlays enabled and has the hdmi-rx device tree overlay active.

For testing purposes you can run it on a laptop with a webcam but that does not provide you with a virtual display.

In the future we will add support for other SBCs like the Raspberry Pi through USB HDMI capture cards

## Dependencies

This requires a linux based OS and installed gcc and build tools.

To compile and run this application, ensure the following libraries are installed:

-   **OpenGL and GLUT libraries**:
    These are essential for creating the window and rendering graphics. On Debian/Ubuntu-based systems, you can install them using:
    ```
    sudo apt update
    sudo apt install freeglut3-dev
    ```

-   **libv4l2 library**:
    This library is required for interacting with V4L2 devices. On Debian/Ubuntu-based systems, install it with:
    ```
    sudo apt install libv4l-dev
    ```

-   **libhidapi-dev** (optional): If your are using the revers engineered protocol instead of the official Viture SDK
    ```
    sudo apt install libhidapi-dev
    ```


## Compilation

Depending on the architecture you are running it on use the reverse engineered protocol version ( ARM ) or the official Viture SDK version ( X86 )

### Using the reverse engineered protocol
```
make
```
This will generate the executable **v4l2_gl**


### Using the official Viture SDK
```
make viture_sdk
```
This will generate the executable **v4l2_gl_viture_sdk**




## Running the viewer

Execute the compiled application from your terminal:
```
sudo ./v4l2_gl [options]
```
or
```
sudo ./v4l2_gl_viture_sdk [options]
```

### Recenter view / rotation reset

Quickly shake your head left/right 3 times to reset the rotation to the center position after the IMU has drifted too far.


### Command-Line Options

The application supports the following command-line options:

-   **`--device <path>`**:
    Specifies the V4L2 device path (e.g., /dev/video0).
    Default: `/dev/video0`.
    Example: `./v4l2_gl --device /dev/video1`

-   **`--fullscreen`**:
    Runs the application in fullscreen mode.
    Default: `false` (disabled).
    Example: `./v4l2_gl --fullscreen`

-   **`--viture`**:
    Enables integration with Viture headset IMU for controlling the rotation of the displayed plane. The Viture SDK and device must be correctly set up.
    Default: `false` (disabled).
    Example: `./v4l2_gl --viture`

-   **`--test-pattern`**:
    Displays a generated test pattern on the plane instead of the live camera feed. Useful for testing rendering and transformations.
    Default: `false` (disabled).
    Example: `./v4l2_gl --test-pattern`

-   **`--plane-distance <distance>`**:
    Sets the distance at which the plane orbits the world origin. `<distance>` is a floating-point value.
    Default: `1.0`.
    Example: `./v4l2_gl --plane-distance 0.5`

-   **`--plane-scale <scale>`**:
    Sets a scale multiplier for the size of the plane. `<scale>` is a floating-point value (must be > 0).
    Default: `1.0` (original size). Values greater than 1.0 enlarge the plane, less than 1.0 shrink it. Values <= 0.0 are reset to 1.0.
    Example: `./v4l2_gl --plane-scale 1.5`

### Combined Example

You can combine these options:
```bash
./v4l2_gl --device /dev/video1 --viture --fullscreen --plane-distance 0.8 --plane-scale 1.2 --test-pattern
```
This command would:
- Use V4L2 device `/dev/video1`.
- Enable Viture IMU.
- Run in fullscreen.
- Set the plane orbit distance to 0.8 units.
- Scale the plane to 120% of its original size.
- Display the test pattern.


## Demo

[![Video demo](https://img.youtube.com/vi/D6w5kAA22Ts/0.jpg)](https://youtu.be/D6w5kAA22Ts)


## TODO

 - [x] Add support for USB HDMI capture cards to support SBCs that don't have HDMI-in like Raspberry PIs
 - [ ] Fix errors in reverse engineered viture SDK
 - [ ] Improve performance of the hdmi texture conversion
 - [ ] Support MJPEG format to increase framerate of USB capture cards
 - [ ] Add quick gesture to recenter the rotation
 - [ ] Add curved screen option
