/*
 * amba_virt_camera.h
 *
 * The one symbol amba-virt-server resolves from libamba-virt-camera.so with
 * dlopen() and dlsym(). It is a function-pointer type, not a prototype: the
 * server never links the library.
 *
 * resource_lua is the path of the whole resource script, vout_lua the path of
 * the vout script, app_img_profile the integer from the live config. The call
 * returns 0 on success and a negative errno on failure.
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _AMBA_VIRT_CAMERA_H_
#define _AMBA_VIRT_CAMERA_H_

#define AMBA_VIRT_CAMERA_APPLY_SYMBOL "amba_virt_camera_apply"

typedef int (*amba_virt_camera_apply_fn)(const char *resource_lua,
                                         const char *vout_lua,
                                         int app_img_profile);

#endif /* _AMBA_VIRT_CAMERA_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
