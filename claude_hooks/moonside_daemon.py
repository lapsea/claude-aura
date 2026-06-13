#!/usr/bin/env python3
"""
Moonside LED daemon — maintains persistent BLE connection and responds
to state changes written to /tmp/moonside_state by moonside_hook.sh.

States: working, idle, input, off
"""

import asyncio
import fcntl
import logging
import os
import signal
import sys
import time

from bleak import BleakClient, BleakScanner, BLEDevice

LOCK_FILE = "/tmp/moonside_daemon.lock"

NUS_TX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NAME_PREFIX = "MOONSIDE"


def build_color_cmd(r: int, g: int, b: int) -> str:
    return f"COLOR{r:03d}{g:03d}{b:03d}"



PID_FILE = "/tmp/moonside_daemon.pid"
STATE_FILE = "/tmp/moonside_state"
LOG_FILE = "/tmp/moonside_daemon.log"

IDLE_TIMEOUT = 30 * 60  # 30 minutes

WORKING_CMD = build_color_cmd(200, 0, 255)    # working: purple
COLOR_IDLE  = build_color_cmd(255, 180, 50)  # idle: orange-yellow
COLOR_INPUT = build_color_cmd(255, 0, 0)     # input: red
BRIGHTNESS_CMD = "BRIGH096"                  # 80% brightness (Moonside range 0-120)

logging.basicConfig(
    filename=LOG_FILE,
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("moonside")


async def _find_connected_nus() -> BLEDevice | None:
    """macOS fallback: find an already-connected NUS peripheral via CoreBluetooth."""
    try:
        from bleak.backends.corebluetooth.scanner import CentralManagerDelegate
        import CoreBluetooth
    except ImportError:
        return None
    delegate = CentralManagerDelegate()
    await delegate.wait_until_ready()
    svc_uuid = CoreBluetooth.CBUUID.UUIDWithString_(NUS_SERVICE_UUID)
    peripherals = delegate.central_manager.retrieveConnectedPeripheralsWithServices_([svc_uuid])
    for p in peripherals:
        name = str(p.name() or "")
        addr = str(p.identifier())
        log.info("Found connected peripheral via CoreBluetooth: %s (%s)", name or "unnamed", addr)
        return BLEDevice(address=addr, name=name, details=(p, delegate))
    return None


async def discover_moonside(timeout: float = 10.0):
    """Scan for a device whose name starts with MOONSIDE or advertises NUS service UUID.
    Falls back to CoreBluetooth connected peripherals on macOS."""
    log.info("Scanning for device named %s* or with NUS service UUID...", NAME_PREFIX)
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for addr, (device, adv_data) in devices.items():
        name = device.name or ""
        if name.upper().startswith(NAME_PREFIX):
            log.info("Found by name: %s (%s)", name, addr)
            return device
        if NUS_SERVICE_UUID in adv_data.service_uuids:
            log.info("Found by UUID: %s (%s)", name or "unnamed", addr)
            return device
    log.warning("Scan found nothing — checking CoreBluetooth connected peripherals...")
    device = await _find_connected_nus()
    if device:
        return device
    log.error("No Moonside/NUS device found")
    sys.exit(1)


async def send(client: BleakClient, cmd: str):
    log.info("TX %s", cmd)
    await client.write_gatt_char(NUS_TX_UUID, cmd.encode("utf-8"), response=True)


async def connect_with_retry(device) -> tuple[BleakClient, object]:
    """Returns (client, device). Re-discovers by name if retries fail."""
    delays = [1, 2, 4, 8, 16]
    for delay in delays:
        try:
            client = BleakClient(device, timeout=15.0)
            await client.connect()
            if client.is_connected:
                log.info("Connected to %s", device.name or device.address)
                return client, device
        except Exception as e:
            log.warning("Connect failed: %s, retry in %ds", e, delay)
            await asyncio.sleep(delay)

    log.warning("All retries exhausted, re-scanning...")
    device = await discover_moonside()
    client = BleakClient(device, timeout=15.0)
    await client.connect()
    return client, device


def read_state() -> str:
    try:
        with open(STATE_FILE) as f:
            return f.read().strip()
    except FileNotFoundError:
        return ""


def cleanup():
    try:
        os.unlink(PID_FILE)
    except OSError:
        pass


async def main():
    # Ensure only one daemon runs at a time
    lock_fp = open(LOCK_FILE, "w")
    try:
        fcntl.flock(lock_fp, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        log.info("Another daemon already running, exiting")
        sys.exit(0)

    device = await discover_moonside()

    # Write PID
    with open(PID_FILE, "w") as f:
        f.write(str(os.getpid()))

    shutdown = asyncio.Event()

    def handle_sigterm(*_):
        shutdown.set()

    signal.signal(signal.SIGTERM, handle_sigterm)
    signal.signal(signal.SIGINT, handle_sigterm)

    client, device = await connect_with_retry(device)
    await send(client, BRIGHTNESS_CMD)

    current_state = ""
    theme_idx = 0
    last_theme_time = 0.0
    idle_since: float | None = None

    try:
        while not shutdown.is_set():
            desired = read_state()

            # State transition
            if desired != current_state:
                log.info("State: %s -> %s", current_state, desired)
                current_state = desired
                idle_since = None

                try:
                    if not client.is_connected:
                        client, device = await connect_with_retry(device)

                    if current_state == "idle":
                        await send(client, "LEDON")
                        await asyncio.sleep(0.3)
                        await send(client, COLOR_IDLE)
                        idle_since = time.monotonic()
                    elif current_state == "input":
                        await send(client, "LEDON")
                        await asyncio.sleep(0.3)
                        await send(client, COLOR_INPUT)
                    elif current_state == "working":
                        await send(client, WORKING_CMD)
                    elif current_state == "off":
                        await send(client, "LEDOFF")
                        break
                except Exception as e:
                    log.error("BLE send error on transition: %s", e)
                    try:
                        client, device = await connect_with_retry(device)
                    except Exception:
                        pass

            # Idle timeout
            if current_state == "idle" and idle_since is not None:
                if time.monotonic() - idle_since >= IDLE_TIMEOUT:
                    log.info("Idle timeout reached, shutting down")
                    try:
                        if client.is_connected:
                            await send(client, "LEDOFF")
                    except Exception:
                        pass
                    break

            await asyncio.sleep(0.2)

    finally:
        # Graceful shutdown
        try:
            if client.is_connected:
                await send(client, "LEDOFF")
                await asyncio.sleep(0.1)
                await client.disconnect()
        except Exception as e:
            log.warning("Shutdown disconnect error: %s", e)
        cleanup()
        log.info("Daemon exited")


if __name__ == "__main__":
    asyncio.run(main())
