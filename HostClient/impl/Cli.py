import asyncio
import logging
from typing import Any, Optional
from .Serial import Serial, SerialRequest, SerialResponse

CLI_DEFAULT_HOST:str = "127.0.0.1"
CLI_DEFAULT_PORT:int = 5001

class Cli:
    def __init__(self, logger:logging.Logger, host:str, port:int, serial:Serial) -> None:
        self._host:str = host
        self._port:int = port
        self._serial:Serial = serial
        self._logger:logging.Logger = logger
        self._queue:asyncio.Queue[SerialResponse] = asyncio.Queue()

        self._server:Optional[asyncio.Server] = None

    async def _handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        addr:Any = writer.get_extra_info("peername")
        self._logger.info(f"Client connected: {addr}")

        writer.write(b"Welcome to watchdog. Type `quit` to exit\r\n")
        while True:
            # Create tasks for both awaits
            task_read_cli:asyncio.Task[Any] = asyncio.create_task(reader.readline())
            task_read_queue:asyncio.Task[Any] = asyncio.create_task(self._queue.get())
            
            done, pending = await asyncio.wait(
                {task_read_cli, task_read_queue},
                return_when=asyncio.FIRST_COMPLETED,
            )

            # Cancel the task that didn't finish
            for task in pending:
                task.cancel()

            result = list(done)[0].result()

            if task_read_cli in done:
                data:bytes = result
                if not data:
                    print("Reader closed")
                    return
                try:
                    cmd = data.decode(encoding='utf-8').strip()
                    if cmd == "quit":
                        writer.write(b"bye\r\n")
                        await writer.drain()
                        break
                    else:
                        serialRequest:SerialRequest = SerialRequest(request=cmd, responseQueue=self._queue)
                        await self._serial.push_request(serialRequest=serialRequest)
                except Exception as e:
                    self._logger.error(f"Error decoding `{data}` from telnet: {e}")


                await writer.drain()

            elif task_read_queue in done:
                serialResponse:SerialResponse = result
                for line in serialResponse.responseLines:
                    writer.write(line.encode(encoding='utf-8')+b'\r\n')



        self._logger.info("Client disconnected")
        writer.close()
        await writer.wait_closed()


    async def run(self) -> None:
        self._server = await asyncio.start_server(self._handle_client, self._host, self._port)
        self._logger.info(f"Listening on {self._host}:{self._port}")

        try:
            async with self._server:
                await asyncio.gather(
                    self._server.serve_forever(),
                )
        except asyncio.CancelledError:
            self._logger.info("Terminating Cli...")
            raise