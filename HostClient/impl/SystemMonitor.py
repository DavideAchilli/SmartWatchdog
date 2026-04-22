# This is in charge of making sure the connection is available
import asyncio
import logging

VCGENCMD:str = '/usr/bin/vcgencmd'

SYSTEM_DEFAULT_POLL_INTERVAL:int = 60 # In seconds

class SystemMonitor:
    def __init__(self, logger:logging.Logger, systemPollPeriodS:int) -> None:
        self._logger:logging.Logger = logger
        self._systemPollPeriodS:int = systemPollPeriodS

    # vcgencmd get_throttled
    # vcgencmd measure_temp
    async def _execute(self, command:list[str]) -> str:
        try:
            proc = await asyncio.create_subprocess_exec(
                *command,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL
            )
            stdout, _ = await proc.communicate()
            return stdout.decode().strip()
        except Exception as e:
            self._logger.error(f'Error executing command [{' '.join(command)}]: {e}')
            return "[ERROR]"

    
    async def run(self) -> None:
        while True:
            measure_temp:str = await self._execute(command=[VCGENCMD, 'measure_temp'])
            get_throttled:str = await self._execute(command=[VCGENCMD, 'get_throttled'])
            self._logger.info(f"System {measure_temp} {get_throttled}")
            await asyncio.sleep(self._systemPollPeriodS)

