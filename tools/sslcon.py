"""Test console: run TimeShift script (SSL) statements in a test game on this PC started with TS_CONSOLE=<port>
(native/console.c compiles them as a function and runs it in the engine's script tick)
and replies with the result or the error. The game listens on 127.0.0.1 only.

Usage: python -I sslcon.py [port] "statements"   (default 28778)
       python -I sslcon.py [port] -              (statements from stdin)
A value comes back with return: sslcon.py "return 1 + 2"
"""
import socket, sys


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    host, port = '127.0.0.1', 28778
    if len(args) > 1:
        port = int(args.pop(0))
    code = sys.stdin.read() if args[0] == '-' else args[0]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(5)
    s.sendto(code.encode('cp1251', 'replace'), (host, port))
    try:
        data, _ = s.recvfrom(65536)
    except socket.timeout:
        print('no answer (is the game running with TS_CONSOLE, past its first script tick?)')
        return 1
    print(data.decode('cp1251', 'replace'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
