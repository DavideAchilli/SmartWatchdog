# Implements the serial port connection
import serial
import asyncio
import logging
SERIAL_DEFAULT_PORT:str = "/dev/ttyUSB0"
SERIAL_DEFAULT_ALIVE_SECONDS:int = 30
import time
from typing import Optional

class SerialResponse:
    def __init__(self, request:str) -> None:
        self.request:str = request
        self.responseLines:list[str] = list()


class SerialRequest:
    def __init__(self, request:str, responseQueue:Optional[asyncio.Queue[SerialResponse]] = None) -> None:
        self.request:str = request
        self.responseQueue:Optional[asyncio.Queue[SerialResponse]] = responseQueue
        self.requestStartTime:float = 0

class Serial:
    def __init__(self, logger:logging.Logger, serialPort:str, aliveSeconds:int) -> None:
        self._logger:logging.Logger = logger
        self._alive:bool = True

        # Open serial port
        self._serial:serial.Serial = serial.Serial(
            port=serialPort,
            baudrate=115200,
            timeout=4,             # seconds
        )

        self._logger.info(f"Connected to port `{serialPort}`")
        self._lastAlive:float = time.monotonic()
        self._aliveSeconds:int = aliveSeconds

        # Queue with all the requests
        self._requestsQueue:asyncio.Queue[SerialRequest] = asyncio.Queue()

        self._currentRequest:Optional[SerialRequest] = None
        self._currentResponse:Optional[SerialResponse] = None

    async def push_request(self, serialRequest:SerialRequest) -> None:
        await self._requestsQueue.put(item=serialRequest)

    async def request_reboot(self) -> None:
        if not self._alive: return
        self._logger.info(f"Reboot requested, sending ALIVE disabled")
        serialRequest:SerialRequest = SerialRequest(request="REBOOT 3")
        self._alive = False
        await self._requestsQueue.put(item=serialRequest)

    def _send_request(self) -> None:
        # Still serving a request
        if self._currentRequest is not None: return
        # No requests
        if self._requestsQueue.empty(): return

        self._currentRequest = self._requestsQueue.get_nowait()
        self._currentRequest.requestStartTime = time.monotonic()
        if self._currentRequest.responseQueue:
            self._currentResponse = SerialResponse(request=self._currentRequest.request)

        self._logger.info(f"SENDING: {self._currentRequest.request}")
        self._serial.write(self._currentRequest.request.encode(encoding='utf-8'))
        self._serial.write(b'\n')


    async def _send_alive(self) -> None:
        now:float = time.monotonic()
        if self._alive and (now - self._lastAlive >= self._aliveSeconds):
            self._lastAlive = now
            await self.push_request(SerialRequest(request='ALIVE'))

    async def _dispatch_line(self, line:str) -> None:
        self._logger.info(f"> {line}")

        # In case of #INFO, send an alive after a few seconds
        if line[:5] == '#INFO':
            now:float = time.monotonic()
            self._lastAlive = now - self._aliveSeconds + 2
        else:
            if line[:4] == '#EOL':
                if self._currentResponse is not None and self._currentRequest is not None and self._currentRequest.responseQueue is not None:
                    try:
                        await self._currentRequest.responseQueue.put(self._currentResponse)
                    except Exception as e:
                        self._logger.error(f"Error sending response: {e}")
                    
                self._currentRequest = None
                self._currentResponse = None
            else:
                if self._currentResponse is not None:
                    self._currentResponse.responseLines.append(line)

    async def poll_serial(self) -> None:
        try:
            binaryLine:bytearray = bytearray()
            while True:
                n:int = self._serial.in_waiting
                if n > 0:
                    chunk = self._serial.read(n)
                    for b in chunk:
                        if b == 13: pass
                        elif b == 10:
                            try:
                                await self._dispatch_line(line = binaryLine.decode(encoding='utf-8'))
                            except Exception as e:
                                self._logger.error(f"Error decoding `{binaryLine}` from serial: {e}")
                            binaryLine.clear()
                        else:
                            binaryLine.append(b)

                await asyncio.sleep(0.01)  # 10 ms poll
                await self._send_alive()
                self._send_request()

        except asyncio.CancelledError:
            self._logger.info("Terminating Serial...")
            raise