#pragma once
// Build identity and immutable family boundary for this separate product.
// This header is also directly testable outside the kernel.
#define IC_PRODUCT_SERVICE L"InterceptorGamepadController"
#define IC_PRODUCT_XUSB_HID_PARENT_VALUE L"InterceptorGamepadControllerXusbHidParent"
#define IC_PRODUCT_XUSB_PARENT_VALUE L"InterceptorGamepadControllerXusbParent"
#define IC_PRODUCT_CONTROL_HWID L"ROOT\\INTERCEPTORGAMEPADCONTROLLER"
#define IC_PRODUCT_NT_DEVICE L"\\Device\\InterceptorGamepadController"
#define IC_PRODUCT_DOS_DEVICE L"\\DosDevices\\InterceptorGamepadController"
#define IC_PRODUCT_TRIAL_SLOT 2u
#define IC_PRODUCT_TRIAL_KEY L"\\Registry\\Machine\\HARDWARE\\InterceptorGamepadControllerTrial"
#ifndef IC_PRODUCT_INPUT_MASK
#define IC_PRODUCT_INPUT_MASK 0u
#endif
#ifndef IC_PRODUCT_CONTROLLER_MASK
#define IC_PRODUCT_CONTROLLER_MASK 15u
#endif
#ifndef IC_PRODUCT_HAS_KEYBOARD_MOUSE
#define IC_PRODUCT_HAS_KEYBOARD_MOUSE 0
#endif
#ifndef IC_PRODUCT_HAS_CONTROLLERS
#define IC_PRODUCT_HAS_CONTROLLERS 1
#endif
#define IC_PRODUCT_STREAM_MASK (IC_PRODUCT_INPUT_MASK | (IC_PRODUCT_CONTROLLER_MASK << 2))
#define IC_PRODUCT_HID_GUID {0x2de3334d,0x67cd,0x4a3a,{0xa2,0x2c,0x04,0x8f,0x77,0xba,0x32,0x3e}}
#define IC_PRODUCT_XUSB_GUID {0x34c1a1e7,0x9f5e,0x4b6c,{0xa7,0x13,0x55,0xb9,0x51,0xea,0xcf,0x35}}
#define IC_PRODUCT_XINPUTHID_GUID {0x7213ba9d,0xe7f2,0x43e6,{0xb3,0x74,0xa5,0xc4,0x77,0x31,0x2c,0x13}}
#define IC_PRODUCT_GIP_GUID {0xc2543732,0xf736,0x49ff,{0xb8,0x4d,0x6e,0xae,0xbd,0xaa,0x46,0xdf}}
#define IC_PRODUCT_XUSB_MIRROR_GUID {0xeed47f20,0x11e1,0x443d,{0xbd,0xe7,0x5d,0xb5,0xc8,0xc8,0x3f,0x1c}}
static __inline int IcProductTypeAllowed(unsigned int type) {
    return type>=1 && type<=6 && (IC_PRODUCT_STREAM_MASK & (1u<<(type-1)))!=0;
}
static __inline int IcProductCreateMasksAllowed(unsigned int input,unsigned int controllers) {
    return !(input & ~IC_PRODUCT_INPUT_MASK) && !(controllers & ~IC_PRODUCT_CONTROLLER_MASK);
}
// Function numbers are the pointer-free protocol's CTL_CODE function field.
// Unknown/common functions keep the normal control dispatch validation.
static __inline int IcProductIoctlFunctionAllowed(unsigned int fn) {
    switch(fn) {
    case 0xA01:case 0xA03:case 0xA07:
        return (IC_PRODUCT_INPUT_MASK & 1u)!=0;
    case 0xA02:case 0xA04:case 0xA08:
        return (IC_PRODUCT_INPUT_MASK & 2u)!=0;
    case 0xA09:case 0xA0A:case 0xA0B:case 0xA0C:case 0xA0D:
        return (IC_PRODUCT_CONTROLLER_MASK & 1u)!=0;
    case 0xA10:case 0xA11:case 0xA12:case 0xA13:case 0xA14:case 0xA15:
        return (IC_PRODUCT_CONTROLLER_MASK & 2u)!=0;
    case 0xA17:case 0xA18:case 0xA19:case 0xA1A:case 0xA1B:
        return (IC_PRODUCT_CONTROLLER_MASK & 4u)!=0;
    case 0xA20:case 0xA21:case 0xA22:case 0xA23:case 0xA24:case 0xA25:
    case 0xA29:case 0xA2A:case 0xA2B:
        return (IC_PRODUCT_CONTROLLER_MASK & 8u)!=0;
    case 0xA30:case 0xA31:case 0xA32:case 0xA33:
        return IC_PRODUCT_HAS_CONTROLLERS;
    default:return 1;
    }
}

