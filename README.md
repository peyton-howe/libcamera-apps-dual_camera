# libcamera-apps
Mofied version of the raspberry pi libcamera-hello that adds distortion via OpenGL shaders to both the X11 and DRM preview. Eventually looking to add stereoscopic support and possibly DRM leasing.

Build
-----
Ensure required dependencies are installed 
```
sudo apt install -y cmake libboost-program-options-dev libdrm-dev libexif-dev
```
Building libcamera-hello
```
cd /path_to_install/
git clone https://github.com/peytonicmaster6/libcamera-apps.git
cd libcamera-apps
mkdir build
cd build
cmake .. -DENABLE_DRM=1 -DENABLE_X11=1 -DENABLE_QT=1 -DENABLE_OPENCV=0 -DENABLE_TFLITE=0
make -j4  # use -j1 on Raspberry Pi 3 or earlier devices
```
Running
-------
To run it in general, use any parameters you would feed to libcamera-hello normally. <br>
To run in X11, just run in the Desktop environment. <br>
To run in DRM, run in console mode. <br>
To see the barrel distortion work correctly, run this command:
```
./libcamera-hello --width 960 --height 1080 -p 0,0,1920,1080 
```

DRM-leasing coming soon <br>

License
-------

The source code is made available under the simplified [BSD 2-Clause license](https://spdx.org/licenses/BSD-2-Clause.html).
