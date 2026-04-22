from impl.Serial import Serial, SERIAL_DEFAULT_ALIVE_SECONDS, SERIAL_DEFAULT_PORT
from impl.Cli import Cli, CLI_DEFAULT_HOST, CLI_DEFAULT_PORT
from impl.NetMonitor import NetMonitor, NET_DEFAULT_MAX_UNREACH, NET_DEFAULT_INTERPING_PAUSE
from impl.SystemMonitor import SystemMonitor, SYSTEM_DEFAULT_POLL_INTERVAL
import argparse
import logging
import sys
import asyncio
from typing import Any

def setup_logging(logfile: str) -> logging.Logger:
    logger:logging.Logger = logging.getLogger()
    logger.setLevel(logging.DEBUG)

    # --- File handler ---
    if logfile:
        fh = logging.FileHandler(logfile, encoding="utf-8")
        fh.setLevel(logging.DEBUG)
        fh.setFormatter(logging.Formatter(
            "%(asctime)s [%(levelname)s] %(name)s: %(message)s"
        ))
        logger.addHandler(fh)

    # --- Stdout handler ---
    sh = logging.StreamHandler(sys.stdout)
    sh.setLevel(logging.INFO)
    sh.setFormatter(logging.Formatter(
        "%(asctime)s [%(levelname)s] %(name)s: %(message)s"
        #"%(levelname)s: %(message)s"
    ))
    logger.addHandler(sh)

    return logger


async def main():

    parser:argparse.ArgumentParser = argparse.ArgumentParser()

    parser.add_argument(
        "--serial",
        help=f"Serial port (default: `{SERIAL_DEFAULT_PORT}`)",
        required=False,
        default=SERIAL_DEFAULT_PORT
    )
    parser.add_argument(
        "--alive",
        type=int,
        help=f"Alive seconds (default: `{SERIAL_DEFAULT_ALIVE_SECONDS}`)",
        required=False,
        default=SERIAL_DEFAULT_ALIVE_SECONDS
    )
    parser.add_argument(
        "--log",
        help=f"Log file name",
        required=False
    )
    parser.add_argument(
        "--cli_host",
        help=f"Listen host name (default: `{CLI_DEFAULT_HOST}`)",
        required=False,
        default=CLI_DEFAULT_HOST
    )
    parser.add_argument(
        "--cli_port",
        type=int,
        help=f"Listen port name (default: `{CLI_DEFAULT_PORT}`)",
        required=False,
        default=CLI_DEFAULT_PORT
    )

    parser.add_argument(
        "--max_net_unreach",
        type=int,
        help=f"If the system is unable to ping any of the indicated addresses for more than this number of seconds, it will reboot; default is {NET_DEFAULT_MAX_UNREACH}",
        required=False,
        default=NET_DEFAULT_MAX_UNREACH
    )

    parser.add_argument(
        "--sys_poll",
        type=int,
        help=f"System poll interval in seconds",
        required=False,
        default=SYSTEM_DEFAULT_POLL_INTERVAL
    )
    
    parser.add_argument(
        "--ping",
        nargs="*",
        help=f"IP addresses to ping",
        required=False,
        default=[]
    )

    parser.add_argument(
        "--ping_wait",
        type=int,
        help=f"Pause between pings in seconds; default is {NET_DEFAULT_INTERPING_PAUSE}",
        required=False,
        default=NET_DEFAULT_INTERPING_PAUSE
    )


    args:argparse.Namespace = parser.parse_args()
    logger:logging.Logger = setup_logging(logfile=args.log)

    try:
        ser:Serial = Serial(logger=logger, serialPort=args.serial, aliveSeconds=args.alive)
    except Exception as e:
        logger.error(f"Failed creating `Serial` object: {e}")
        exit(1)

    cli:Cli = Cli(logger=logger, host=args.cli_host, port=args.cli_port, serial=ser)
    systemMonitor:SystemMonitor = SystemMonitor(logger=logger, systemPollPeriodS=args.sys_poll)

    # Create tasks so they run concurrently
    tasks:list[asyncio.Task[Any]] = [
        asyncio.create_task(ser.poll_serial()),
        asyncio.create_task(cli.run()),
        asyncio.create_task(systemMonitor.run()),
    ]

    if len(args.ping) > 0:
        netMonitor:NetMonitor = NetMonitor(logger=logger, serial=ser, pingables=args.ping, max_unreach_s=args.max_net_unreach, interping_pause_s=args.ping_wait)
        tasks.append(asyncio.create_task(netMonitor.run()))

    # Wait for both forever (or until cancelled)
    await asyncio.gather(*tasks)

try:
    asyncio.run(main())
except Exception as e:
    print(f"TERMINATING WITH EXCEPTION: {e}")
