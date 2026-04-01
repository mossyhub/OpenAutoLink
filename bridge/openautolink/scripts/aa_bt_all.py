import dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib
import os, time, threading, struct, fcntl

AA_UUID = "4de17a00-52cb-11e6-bdf4-0800200c9a66"
HSP_HS_UUID = "00001108-0000-1000-8000-00805f9b34fb"
HSP_AG_UUID = "00001112-0000-1000-8000-00805f9b34fb"
AGENT_IFACE = "org.bluez.Agent1"
PROFILE_IFACE = "org.bluez.Profile1"
LE_AD_IFACE = "org.bluez.LEAdvertisement1"
AA_CHANNEL = 8

# WiFi credentials — override via environment or edit here
WIFI_SSID = os.environ.get("OAL_WIRELESS_SSID", os.environ.get("PI_AA_WIRELESS_SSID", "")) or "OpenAutoLink"
WIFI_KEY = os.environ.get("OAL_WIRELESS_PASSWORD", os.environ.get("PI_AA_WIRELESS_PASSWORD", "")) or "openautolink"
WIFI_IP = "192.168.43.1"
WIFI_PORT = int(os.environ.get("OAL_PHONE_TCP_PORT", os.environ.get("PI_AA_BACKEND_TCP_PORT", "5277")))
WIFI_BSSID = "00:00:00:00:00:00"  # filled at runtime from wlan0 MAC

# ---- Minimal protobuf encoding (no library needed) ----
def _varint(v):
    r = []
    while v > 0x7f:
        r.append((v & 0x7f) | 0x80); v >>= 7
    r.append(v & 0x7f)
    return bytes(r)

def pbs(field_num, value):
    e = value.encode("utf-8")
    return bytes([(field_num << 3) | 2]) + _varint(len(e)) + e

def pbi(field_num, value):
    return bytes([(field_num << 3) | 0]) + _varint(value)

def rfcomm_send(fd, msg_type, payload):
    h = struct.pack(">HH", len(payload), msg_type)
    os.write(fd, h + payload)

def rfcomm_recv(fd):
    h = b""
    while len(h) < 4:
        h += os.read(fd, 4 - len(h))
    sz, mt = struct.unpack(">HH", h)
    d = b""
    while len(d) < sz:
        d += os.read(fd, sz - len(d))
    return mt, d

def handle_aa_rfcomm(fd):
    """Handle the AA wireless WiFi credential exchange over RFCOMM."""
    fl = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, fl & ~os.O_NONBLOCK)

    try:
        print(f"Sending WifiStartRequest ip={WIFI_IP} port={WIFI_PORT}", flush=True)
        rfcomm_send(fd, 1, pbs(1, WIFI_IP) + pbi(2, WIFI_PORT))

        mt, d = rfcomm_recv(fd)
        print(f"Got msg type={mt} len={len(d)}", flush=True)

        print(f"Sending WifiInfoResponse ssid={WIFI_SSID}", flush=True)
        rfcomm_send(fd, 3, pbs(1, WIFI_SSID) + pbs(2, WIFI_KEY) + pbs(3, WIFI_BSSID) + pbi(4, 8) + pbi(5, 1))

        mt2, d2 = rfcomm_recv(fd)
        print(f"Got msg type={mt2} len={len(d2)}", flush=True)

        mt3, d3 = rfcomm_recv(fd)
        print(f"Got msg type={mt3} len={len(d3)}", flush=True)

        print("WiFi credential exchange complete!", flush=True)
    except Exception as e:
        print(f"RFCOMM exchange error: {e}", flush=True)
    finally:
        os.close(fd)


class Agent(dbus.service.Object):
    @dbus.service.method(AGENT_IFACE, in_signature="ou", out_signature="")
    def RequestConfirmation(self, d, p):
        print(f"Auto-confirm {d} passkey={p}", flush=True)
        # Return without error = auto-accept
    @dbus.service.method(AGENT_IFACE, in_signature="o", out_signature="s")
    def RequestPinCode(self, d):
        print(f"Auto-pin {d} -> 123456", flush=True)
        return "123456"
    @dbus.service.method(AGENT_IFACE, in_signature="o", out_signature="u")
    def RequestPasskey(self, d):
        print(f"Auto-passkey {d}", flush=True)
        return dbus.UInt32(0)
    @dbus.service.method(AGENT_IFACE, in_signature="ou", out_signature="")
    def DisplayPasskey(self, d, p):
        print(f"Display passkey {d} {p}", flush=True)
    @dbus.service.method(AGENT_IFACE, in_signature="os", out_signature="")
    def DisplayPinCode(self, d, p):
        print(f"Display pin {d} {p}", flush=True)
    @dbus.service.method(AGENT_IFACE, in_signature="o", out_signature="")
    def RequestAuthorization(self, d):
        print(f"Auto-authorize {d}", flush=True)
    @dbus.service.method(AGENT_IFACE, in_signature="os", out_signature="")
    def AuthorizeService(self, d, u):
        print(f"Auto-authorize-svc {d} {u}", flush=True)
    @dbus.service.method(AGENT_IFACE, in_signature="", out_signature="")
    def Release(self): pass
    @dbus.service.method(AGENT_IFACE, in_signature="", out_signature="")
    def Cancel(self): pass

class AAProfile(dbus.service.Object):
    @dbus.service.method(PROFILE_IFACE, in_signature="oha{sv}", out_signature="")
    def NewConnection(self, device, fd, props):
        fd = fd.take()
        print(f"AA RFCOMM NewConnection from {device} fd={fd}", flush=True)
        threading.Thread(target=handle_aa_rfcomm, args=(fd,), daemon=True).start()
    @dbus.service.method(PROFILE_IFACE, in_signature="o", out_signature="")
    def RequestDisconnection(self, dev): print(f"AA disconnect {dev}", flush=True)
    @dbus.service.method(PROFILE_IFACE, in_signature="", out_signature="")
    def Release(self): print("AA Released", flush=True)

class HSPProfile(dbus.service.Object):
    @dbus.service.method(PROFILE_IFACE, in_signature="oha{sv}", out_signature="")
    def NewConnection(self, device, fd, props):
        print(f"HSP NewConnection from {device}", flush=True)
    @dbus.service.method(PROFILE_IFACE, in_signature="o", out_signature="")
    def RequestDisconnection(self, dev): print(f"HSP disconnect {dev}", flush=True)
    @dbus.service.method(PROFILE_IFACE, in_signature="", out_signature="")
    def Release(self): print("HSP Released", flush=True)

class BLEAd(dbus.service.Object):
    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="ss", out_signature="v")
    def Get(self, i, p): return self.GetAll(i)[p]
    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="s", out_signature="a{sv}")
    def GetAll(self, i):
        return {"Type": dbus.String("peripheral"), "ServiceUUIDs": dbus.Array([AA_UUID], signature="s"), "LocalName": dbus.String("OpenAutoLink")}
    @dbus.service.method(LE_AD_IFACE, in_signature="", out_signature="")
    def Release(self): pass

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SystemBus()

# Agent
agent = Agent(bus, "/pi_aa/agent")
am = dbus.Interface(bus.get_object("org.bluez", "/org/bluez"), "org.bluez.AgentManager1")
try:
    am.RegisterAgent("/pi_aa/agent", "NoInputNoOutput")
    am.RequestDefaultAgent("/pi_aa/agent")
    print("Agent registered", flush=True)
except dbus.exceptions.DBusException as e:
    print(f"Agent registration: {e} (continuing)", flush=True)

# AA Profile (channel 8)
aa = AAProfile(bus, "/pi_aa/aa")
pm = dbus.Interface(bus.get_object("org.bluez", "/org/bluez"), "org.bluez.ProfileManager1")
sdp = ('<?xml version="1.0" encoding="UTF-8" ?><record>'
    '<attribute id="0x0001"><sequence>'
    '<uuid value="' + AA_UUID + '" /><uuid value="0x1101" />'
    '</sequence></attribute>'
    '<attribute id="0x0004"><sequence>'
    '<sequence><uuid value="0x0100" /></sequence>'
    '<sequence><uuid value="0x0003" /><uint8 value="0x08" /></sequence>'
    '</sequence></attribute>'
    '<attribute id="0x0005"><sequence><uuid value="0x1002" /></sequence></attribute>'
    '<attribute id="0x0009"><sequence><sequence>'
    '<uuid value="0x1101" /><uint16 value="0x0102" />'
    '</sequence></sequence></attribute>'
    '<attribute id="0x0100"><text value="Android Auto Wireless" /></attribute>'
    '<attribute id="0x0101"><text value="AndroidAuto WiFi projection automatic setup" /></attribute>'
    '</record>')
try:
    pm.RegisterProfile("/pi_aa/aa", AA_UUID, {
        "Name": "AA Wireless", "Role": "server",
        "Channel": dbus.UInt16(AA_CHANNEL), "AutoConnect": True,
        "RequireAuthentication": False, "RequireAuthorization": False,
        "ServiceRecord": sdp})
    print(f"AA profile ch={AA_CHANNEL}", flush=True)
except dbus.exceptions.DBusException as e:
    print(f"AA profile: {e} (continuing)", flush=True)

# HSP HS Profile
hsp = HSPProfile(bus, "/pi_aa/hsp")
try:
    pm.RegisterProfile("/pi_aa/hsp", HSP_HS_UUID, {"Name": "HSP HS"})
    print("HSP HS profile", flush=True)
except dbus.exceptions.DBusException as e:
    print(f"HSP profile: {e} (continuing)", flush=True)

# BLE Advertisement
ble = BLEAd(bus, "/pi_aa/ble")
objs = dbus.Interface(bus.get_object("org.bluez", "/"), "org.freedesktop.DBus.ObjectManager").GetManagedObjects()
for p, i in objs.items():
    if "org.bluez.LEAdvertisingManager1" in i:
        dbus.Interface(bus.get_object("org.bluez", p), "org.bluez.LEAdvertisingManager1").RegisterAdvertisement(
            "/pi_aa/ble", {},
            reply_handler=lambda: print("BLE AD ok", flush=True),
            error_handler=lambda e: print(f"BLE AD err: {e}", flush=True))
        break

# Adapter settings
ap = dbus.Interface(bus.get_object("org.bluez", "/org/bluez/hci0"), "org.freedesktop.DBus.Properties")
ap.Set("org.bluez.Adapter1", "Powered", dbus.Boolean(True))
ap.Set("org.bluez.Adapter1", "Discoverable", dbus.Boolean(True))
ap.Set("org.bluez.Adapter1", "Pairable", dbus.Boolean(True))
ap.Set("org.bluez.Adapter1", "DiscoverableTimeout", dbus.UInt32(0))
ap.Set("org.bluez.Adapter1", "Alias", "OpenAutoLink")
# Set device class to Car Audio (0x200418) for proper phone recognition
# Keep SSP enabled — modern phones require it. JustWorks pairing is handled
# by our NoInputNoOutput agent which auto-accepts without any PIN prompt.
os.system("hciconfig hci0 class 0x200418 2>/dev/null")
os.system("hciconfig hci0 sspmode 1 2>/dev/null")
print("Adapter set (class=0x200418 Car Audio, SSP=on)", flush=True)

# Try to read wlan0 BSSID at startup
try:
    with open("/sys/class/net/wlan0/address") as f:
        WIFI_BSSID = f.read().strip().upper()
    print(f"WiFi BSSID: {WIFI_BSSID}", flush=True)
except Exception:
    pass

# Connect to first paired phone's HSP AG after delay
def do_connect():
    time.sleep(5)
    try:
        obj_mgr = dbus.Interface(bus.get_object("org.bluez", "/"),
                                 "org.freedesktop.DBus.ObjectManager")
        objects = obj_mgr.GetManagedObjects()
        for path, ifaces in objects.items():
            if "org.bluez.Device1" in ifaces:
                props = ifaces["org.bluez.Device1"]
                if props.get("Paired", False):
                    print(f"Found paired device: {path}", flush=True)
                    dev = dbus.Interface(bus.get_object("org.bluez", path),
                                        "org.bluez.Device1")
                    dp = dbus.Interface(bus.get_object("org.bluez", path),
                                        "org.freedesktop.DBus.Properties")
                    dp.Set("org.bluez.Device1", "Trusted", dbus.Boolean(True))
                    try:
                        dev.ConnectProfile(HSP_AG_UUID)
                        print(f"Connected HSP AG to {path}", flush=True)
                    except Exception as e2:
                        print(f"HSP connect {path}: {e2}", flush=True)
                    break
    except Exception as e:
        print(f"Phone connect: {e}", flush=True)

threading.Thread(target=do_connect, daemon=True).start()
print("All services running", flush=True)
GLib.MainLoop().run()
