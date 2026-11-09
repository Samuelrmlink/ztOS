#!/usr/bin/env python3
"""Bluetooth for Android apps: put the shim and the Bluetooth app into the
Waydroid images (docs/waydroid.md, "Bluetooth for Android apps").

    patch-bluetooth.py system SYSTEM_ROOT [PREBUILTS]
    patch-bluetooth.py vendor VENDOR_ROOT

SYSTEM_ROOT is a mounted, writable copy of system.img (strip-image.sh's);
VENDOR_ROOT one of vendor.img (patch-vendor.sh's). PREBUILTS holds
Bluetooth.apk and libbluetooth_jni.so (default: the package's
/usr/share/ztos-waydroid/bt, or the checkout's prebuilts/waydroid-bt).

system: in the flattened com.android.btservices APEX, the stock
libbluetooth_jni.so becomes libbluetooth_jni_stock.so and the shim's wrapper
takes its name; Bluetooth.apk goes to app/Bluetooth/. Every file is pinned by
sha256: an unknown stock library (another image) or prebuilt is refused. And
product/etc/default-permissions/ztos-bluetooth_default-permissions.xml grants
the app its Nearby-devices
permissions on a fresh set-up. (An image whose Android already booted once
doesn't apply default grants again: there, `pm grant com.android.bluetooth
android.permission.BLUETOOTH_CONNECT` and _SCAN, _ADVERTISE, once.)
vendor: the android.hardware.bluetooth and bluetooth_le features, without
which system_server starts no Bluetooth service and apps see none.

Both are idempotent. Exit 3: the image isn't the one this was made for.
"""
import hashlib
import os
import shutil
import sys

STOCK_JNI = "9b6dc6ff611abe505356e588b2641483d2237738ec166268751f82e8a90e9e3f"
WRAPPER = "95c93a515c6413665bdc61e69d99743e26f0f61b81d77f760a00b21b5d615922"
APP = "9c19c7d494699b64fa20fea16029efaf11a7c67b37b4a01bd44f8143780bdfbf"
OLD_WRAPPERS = {   # earlier builds of the wrapper, replaced in place
    "c32ddd2f473a19c56413e08dc6766e4ee16957e99ca2477f370a8ba631583557",   # stage 1 (no sockets)
    "e35642447ddf142fea5bd7b4ea154c08bf5bcadf62ebfdba35a85b8611afeff8",   # + RFCOMM (the phone, 8 Nov)
}
APEX = "system/apex/com.android.btservices"
FEATURES = {
    "android.hardware.bluetooth.xml": "android.hardware.bluetooth",
    "android.hardware.bluetooth_le.xml": "android.hardware.bluetooth_le",
}
XATTR = "security.selinux"
# The Bluetooth app's Nearby-devices runtime permissions. Without them its
# ProfileService refuses to start ("Permission denied!"), GattService never
# runs and Android never asks the stack to enable. A stock build grants them
# by its default-grant policy; this file does the same for our added app, at
# the first boot of a fresh set-up (and on a build fingerprint change).
# product's directory: the stock image has one there (and none in system/etc)
DEFAULT_PERMISSIONS = "system/product/etc/default-permissions/ztos-bluetooth_default-permissions.xml"
DEFAULT_PERMISSIONS_XML = """<?xml version="1.0" encoding="utf-8"?>
<!-- ztOS: Bluetooth through BlueZ (ztos-btbridge); docs/waydroid.md -->
<exceptions>
    <exception package="com.android.bluetooth">
        <permission name="android.permission.BLUETOOTH_CONNECT" fixed="true"/>
        <permission name="android.permission.BLUETOOTH_SCAN" fixed="true"/>
        <permission name="android.permission.BLUETOOTH_ADVERTISE" fixed="true"/>
    </exception>
</exceptions>
"""


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def like(ref, path):
    """Mode, owner and SELinux label of ref onto path."""
    st = os.stat(ref)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    try:
        os.setxattr(path, XATTR, os.getxattr(ref, XATTR))
    except OSError:
        pass


def put(src, dst, ref, digest):
    tmp = dst + ".ztbt"
    shutil.copyfile(src, tmp)
    if sha(tmp) != digest:
        os.unlink(tmp)
        raise SystemExit("%s: copy doesn't match its pin" % dst)
    like(ref, tmp)
    os.chmod(tmp, 0o644)
    os.replace(tmp, dst)


def system(root, prebuilts):
    apex = os.path.join(root, APEX)
    lib = os.path.join(apex, "lib64")
    jni = os.path.join(lib, "libbluetooth_jni.so")
    stock = os.path.join(lib, "libbluetooth_jni_stock.so")
    if not os.path.isfile(jni):
        print("no %s: not the pinned image" % jni, file=sys.stderr)
        return 3
    for name, digest in (("libbluetooth_jni.so", WRAPPER), ("Bluetooth.apk", APP)):
        p = os.path.join(prebuilts, name)
        if not os.path.isfile(p) or sha(p) != digest:
            raise SystemExit("%s: missing or not the pinned build" % p)
    have = sha(jni)
    if have == STOCK_JNI and not os.path.exists(stock):
        os.rename(jni, stock)
        put(os.path.join(prebuilts, "libbluetooth_jni.so"), jni, stock, WRAPPER)
        print("libbluetooth_jni.so: the shim's wrapper; the stock one is libbluetooth_jni_stock.so")
    elif have == WRAPPER and os.path.isfile(stock) and sha(stock) == STOCK_JNI:
        print("libbluetooth_jni.so: already the wrapper")
    elif have in OLD_WRAPPERS and os.path.isfile(stock) and sha(stock) == STOCK_JNI:
        put(os.path.join(prebuilts, "libbluetooth_jni.so"), jni, stock, WRAPPER)
        print("libbluetooth_jni.so: wrapper updated")
    else:
        print("libbluetooth_jni.so: unknown (%s): not the pinned image" % have[:16], file=sys.stderr)
        return 3
    appdir = os.path.join(apex, "app", "Bluetooth")
    for d in (os.path.join(apex, "app"), appdir):
        if not os.path.isdir(d):
            os.mkdir(d)
            like(lib, d)
    apk = os.path.join(appdir, "Bluetooth.apk")
    if os.path.isfile(apk) and sha(apk) == APP:
        print("Bluetooth.apk: already there")
    else:
        put(os.path.join(prebuilts, "Bluetooth.apk"), apk, stock, APP)
        print("Bluetooth.apk: added to the btservices APEX")
    default_permissions(root)
    return 0


def default_permissions(root):
    path = os.path.join(root, DEFAULT_PERMISSIONS)
    d = os.path.dirname(path)
    etc = os.path.dirname(d)
    if not os.path.isdir(d):
        os.mkdir(d)
        if os.path.isdir(etc):
            like(etc, d)
    if os.path.isfile(path) and open(path).read() == DEFAULT_PERMISSIONS_XML:
        print("default-permissions: already there")
        return
    with open(path + ".ztbt", "w") as f:
        f.write(DEFAULT_PERMISSIONS_XML)
    ref = next((os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith(".xml") and not f.endswith(".ztbt")),
               None)
    like(ref or etc, path + ".ztbt")
    os.chmod(path + ".ztbt", 0o644)
    os.replace(path + ".ztbt", path)
    print("default-permissions: Nearby-devices grants for com.android.bluetooth")


def vendor(root):
    perms = os.path.join(root, "etc", "permissions")
    if not os.path.isdir(perms):
        print("no %s: not a vendor image" % perms, file=sys.stderr)
        return 3
    ref = next((os.path.join(perms, f) for f in sorted(os.listdir(perms)) if f.endswith(".xml")), None)
    for name, feature in FEATURES.items():
        path = os.path.join(perms, name)
        text = ('<?xml version="1.0" encoding="utf-8"?>\n<!-- ztOS: Bluetooth through BlueZ (ztos-btbridge) -->\n'
                '<permissions>\n    <feature name="%s" />\n</permissions>\n' % feature)
        if os.path.isfile(path) and open(path).read() == text:
            continue
        with open(path + ".ztbt", "w") as f:
            f.write(text)
        if ref:
            like(ref, path + ".ztbt")
        os.chmod(path + ".ztbt", 0o644)
        os.replace(path + ".ztbt", path)
        print("vendor: %s" % name)
    return 0


def default_prebuilts():
    here = os.path.dirname(os.path.realpath(__file__))
    for d in ("/usr/share/ztos-waydroid/bt", os.path.join(here, "..", "..", "prebuilts", "waydroid-bt")):
        if os.path.isfile(os.path.join(d, "Bluetooth.apk")):
            return d
    raise SystemExit("no Bluetooth prebuilts found")


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("system", "vendor"):
        raise SystemExit(__doc__)
    if sys.argv[1] == "system":
        return system(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else default_prebuilts())
    return vendor(sys.argv[2])


if __name__ == "__main__":
    sys.exit(main())
