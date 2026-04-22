# This is in charge of making sure the connection is available
import asyncio
import platform
import time
import logging
from typing import Optional
from .Serial import Serial

NET_DEFAULT_MAX_UNREACH:int = 60*10
NET_DEFAULT_INTERPING_PAUSE:int = 60

class NetMonitor:
    def __init__(self, logger:logging.Logger, serial:Serial, pingables:list[str], max_unreach_s:int, interping_pause_s:int) -> None:
        self._logger:logging.Logger = logger
        self._serial:Serial = serial
        self._pingables:list[str] = list()
        self._last_valid_ping:float = time.monotonic()
        self._max_unreach_s:int = max_unreach_s
        self._interping_pause_s:int = interping_pause_s
        for pingable in pingables:
            self._pingables.append(pingable)


    async def _ping(self, host:str) -> Optional[int]:
        proc = await asyncio.create_subprocess_exec(
            "ping", ("-n" if platform.system() == "Windows" else "-c"), "1", host,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.DEVNULL
        )
        await proc.wait()
        return proc.returncode
    
    async def run(self) -> None:
        while True:
            for pingable in self._pingables:
                ret = await self._ping(host=pingable)
                now:float = time.monotonic()
                if ret == 0:
                    self._last_valid_ping = now
                    self._logger.info(f"Ping of {pingable} ok")
                else:
                    self._logger.error(f"Ping of {pingable} failed")
                    elapsed:float = now - self._last_valid_ping
                    if elapsed > self._max_unreach_s:
                        self._logger.error(f"No network: reboot")
                        await self._serial.request_reboot()
                await asyncio.sleep(self._interping_pause_s)

