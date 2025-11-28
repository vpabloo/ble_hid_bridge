#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
import sys
import time


# Enforce virtual environment usage
if sys.prefix == sys.base_prefix:
    print("Error: This script must be run inside a virtual environment (venv).", file=sys.stderr)
    print("Please create and activate a venv before running:", file=sys.stderr)
    print("  python3 -m venv .venv", file=sys.stderr)
    print("  source .venv/bin/activate", file=sys.stderr)
    sys.exit(1)


import serial
import serial.tools.list_ports


# Check for common environment issue where 'serial' package is installed instead of 'pyserial'
if not hasattr(serial, "Serial"):
    print("Error: The installed 'serial' module does not contain the 'Serial' class.", file=sys.stderr)
    print("This usually means you have the wrong 'serial' package installed (likely 'serial' instead of 'pyserial').", file=sys.stderr)
    print("Please run the following commands to fix it:", file=sys.stderr)
    print("  pip uninstall -y serial", file=sys.stderr)
    print("  pip install --force-reinstall pyserial", file=sys.stderr)
    sys.exit(1)


CACHE_PATH = Path(__file__).resolve().parent / "bhid.json"
KNOWN_VID_PID = {(0x0483, 0x5740)}  # STM32 VCP used by Flipper em modo normal


def _load_cached_port() -> str | None:
    try:
        data = json.loads(CACHE_PATH.read_text())
        return data.get("port")
    except Exception:
        return None


def _save_cached_port(port: str, reason: str) -> None:
    try:
        CACHE_PATH.write_text(
            json.dumps({"port": port, "reason": reason, "saved_at": int(time.time())}, indent=2)
        )
    except Exception:
        pass


def _port_match_reason(port) -> str:
    """Retorna motivo textual se o port parecer ser o Flipper."""
    if port.vid is not None and port.pid is not None:
        if (port.vid, port.pid) in KNOWN_VID_PID:
            return "VID:PID 0483:5740"

    desc_parts = [port.manufacturer or "", port.product or "", port.description or ""]
    desc = " ".join(desc_parts).lower()
    if "flipper" in desc:
        return "Descrição contém 'Flipper'"

    path = (port.device or "").lower()
    if "usbmodemflip" in path or "flipper" in path:
        return "Caminho da porta contém 'flipper'"

    return None


def find_flipper_port(ports=None, verbose: bool = False) -> tuple[str | None, str | None]:
    """Tenta encontrar o Flipper Zero via heurísticas de porta serial USB."""
    if ports is None:
        ports = list(serial.tools.list_ports.comports())

    for port in ports:
        reason = _port_match_reason(port)
        if reason:
            return port.device, reason

    if verbose and ports:
        print("Portas encontradas, nenhuma parece ser o Flipper:")
        for port in ports:
            meta = ", ".join(
                filter(None, [port.device, port.manufacturer, port.product, port.description])
            )
            print(f"  - {meta}")

    return None, None


def send_line(ser: serial.Serial, line: str) -> None:
    sent = ser.write((line + "\n").encode("utf-8"))
    ser.flush()
    return sent


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Envia comandos de mouse/teclado para o Flipper via Serial (USB)."
    )
    parser.add_argument("--port", "-p", help="Porta Serial do Flipper (opcional)")
    parser.add_argument("--baudrate", type=int, default=115200, help="Baudrate (padrão 115200)")
    parser.add_argument("--verbose", "-v", action="store_true", help="Exibe comandos enviados")
    subparsers = parser.add_subparsers(dest="cmd", required=False)

    move_p = subparsers.add_parser("move", help="Mover mouse relativo")
    move_p.add_argument("dx", type=int, help="Delta X (-127..127)")
    move_p.add_argument("dy", type=int, help="Delta Y (-127..127)")

    btn_p = subparsers.add_parser("btn", help="Definir máscara de botões (1=esq,2=dir,4=meio)")
    btn_p.add_argument("mask", type=int, help="Bitmask desejada")

    scroll_p = subparsers.add_parser("scroll", help="Scroll vertical/horizontal")
    scroll_p.add_argument("v", type=int, help="Vertical -127..127")
    scroll_p.add_argument("h", type=int, help="Horizontal -127..127")

    type_p = subparsers.add_parser("type", help="Digitar texto ASCII")
    type_p.add_argument(
        "text",
        nargs=argparse.REMAINDER,
        help="Texto a ser digitado (omitido => 'hello flipper')",
        default=None,
    )

    key_p = subparsers.add_parser("key", help="Enviar HID code bruto")
    key_p.add_argument("code", type=int, help="Código HID da tecla")

    shell_p = subparsers.add_parser("shell", help="Modo interativo (shell)")

    args = parser.parse_args()

    # Se nenhum subcomando for informado, assume "type" com mensagem padrão
    if not args.cmd:
        args.cmd = "type"
        if not hasattr(args, "text"):
            args.text = None

    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("Nenhuma porta serial encontrada.")
        return 1

    port_to_use = args.port
    cached_reason = "cache" if _load_cached_port() else None

    # Tenta usar porta em cache antes de listar
    if not port_to_use:
        cached = _load_cached_port()
        if cached and any(p.device == cached for p in ports):
            port_to_use = cached
            if args.verbose:
                print(f"Usando porta em cache: {cached}")
        else:
            cached_reason = None

    # Se ainda não há porta definida, lista e detecta
    if not port_to_use:
        print("Portas USB detectadas:")
        for port in ports:
            meta = ", ".join(
                filter(None, [port.device, port.manufacturer, port.product, port.description])
            )
            print(f"  - {meta}")

        print("Procurando Flipper Zero...")
        found, reason = find_flipper_port(ports=ports, verbose=True)
        if not found:
            print("Erro: Flipper Zero não encontrado automaticamente e nenhuma porta especificada.")
            return 1
        print(f"Flipper encontrado em: {found} ({reason})")
        _save_cached_port(found, reason)
        port_to_use = found
        cached_reason = reason

    # Se porta foi fornecida manualmente ou via cache, garante salvar para evitar novas buscas
    if port_to_use and cached_reason:
        _save_cached_port(port_to_use, cached_reason)
    elif port_to_use and not args.port:
        _save_cached_port(port_to_use, "detected")

    with serial.Serial(port_to_use, args.baudrate, timeout=1) as ser:
        if args.cmd == "move":
            line = f"M MOVE {args.dx} {args.dy}"
        elif args.cmd == "btn":
            line = f"M BTN {args.mask}"
        elif args.cmd == "scroll":
            line = f"M SCROLL {args.v} {args.h}"
        elif args.cmd == "type":
            text = " ".join(args.text).strip() if args.text else "hello flipper"
            line = f"K TYPE {text}"
        elif args.cmd == "key":
            line = f"K KEY {args.code}"

        if args.verbose:
            print(f"Enviando para {port_to_use} @ {args.baudrate}: {line}")
        sent = send_line(ser, line)
        if args.verbose:
            print(f"Bytes enviados: {sent}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
