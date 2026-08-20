import asyncio, websockets, argparse, json, time

clients = set()

async def handler(ws):
    clients.add(ws)
    peer = getattr(ws, "remote_address", ("?", "?"))
    print(f"[HUB] + client {peer}")
    try:
        async for msg in ws:
            # basic health: respond to ping JSON or raw string
            try:
                data = json.loads(msg)
                if isinstance(data, dict) and data.get("type") == "ping":
                    await ws.send(json.dumps({"type":"pong","ts":time.time()}))
                    continue
            except Exception:
                if msg.strip().lower() == "ping":
                    await ws.send("pong")
                    continue

            # Broadcast everything else to all others
            for c in list(clients):
                if c.open and c is not ws:
                    await c.send(msg)
    except Exception as e:
        print("[HUB] client error:", e)
    finally:
        clients.discard(ws)
        print(f"[HUB] - client {peer}")

async def main(host, port):
    print(f"[HUB] Listening on ws://{host}:{port}")
    async with websockets.serve(handler, host, port, ping_interval=20, ping_timeout=20, max_size=2**20):
        await asyncio.Future()

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--port", type=int, default=81)
    args = p.parse_args()
    asyncio.run(main(args.host, args.port))
