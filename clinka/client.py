#!/usr/bin/env python3

import paho.mqtt.client as mqtt
import time
import asyncio
import json
import subprocess
import telnetlib
import sqlite3
import sys
import random

# MQTT broker credentials and settings
broker_address = '3a4f7d6b0cd1473681d6c9bdfa569318.s2.eu.hivemq.cloud'
mqtt_port = 8883
ws_port = 8884
username = 'onverantwoordelik'
password = 'asdf8090ABC!!'
use_websockets = False  # Set to False for MQTT over SSL/TLS (port 8883)
client_id="osmobb"
peer_id="nitb"
sub=F'cheapray/{client_id}'
pub=F'cheapray/{peer_id}'
broker_timeout=3600
osmobb_cfg_dir="../src/host/layer23/src/mobile/"

MESSAGE = {
    'type':'cmd',
    'msg':'',
    'is_res': False,
    'is_json': False
}

def configure_ms(data):
    ms_temp = open(F'{osmobb_cfg_dir}default_tmp_ms.cfg').read()
    base_temp = open(F'{osmobb_cfg_dir}default_tmp_ms.cfg').read()
    if 'imsi' in data.keys():
        ms_temp.replace('{{imsi}}', data['imsi'])
        ms_temp.replace('{{imei}}', random.sample('0123456789' * 100, 4))
        ms_temp.replace('{{mcc}}', data['imsi'][:3])
        ms_temp.replace('{{mnc}}', data['imsi'][3:5])
        base_temp.replace('{{mobile_0}}', ms_temp)
        out = open(F'{osmobb_cfg_dir}default.cfg', 'w')
        out.write(base_temp)


def handle_cmd(message, client):
    conn = telnetlib.Telnet("127.0.0.1", 4247)
    conn.read_until(b"OsmocomBB(mobile)>")

    command = json.loads(message)
    if command['sres']:
        conn.write(F"sres 1 {command['sres']}\n".encode())
        res = conn.read_until(b"OsmocomBB(mobile)>")
        print(res.decode())
        conn.close()

def execute_command(command):
    try:
        result = subprocess.run(command, shell=True, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return F"$: {result.stdout.strip()}"
    except subprocess.CalledProcessError as e:
        return f"Error executing command: {e}"

# Define callback functions
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("Connected to MQTT broker")
        client.subscribe(sub)  # Replace with the desired topic to subscribe to
        MESSAGE['msg'] = F'{peer_id}?'
        message = json.dumps(MESSAGE)
        client.publish(pub, message)
    else:
        print("Connection failed")

def on_message(client, userdata, message):
    msg, topic = message.payload.decode(), message.topic
    try:
        msgg = json.loads(msg)
        if msgg['type'] == 'usr':
            if not msgg['is_res']:
                msgg['msg'] = execute_command(msgg['msg'])
                msgg['is_res'] = True
                client.publish(pub, json.dumps(msgg))
            elif msgg['is_res'] :
                if conns:
                    if 'telnet' in conns.keys():
                        #conns['telnet'][1].write(message['msg'])
                        #conns['telnet'][1].drain()
                        pass
                if type(msgg['msg']) == list:
                    print("\x1b[2J")
                    for item in msgg['msg']:
                        print(F"{item} \n")
                else:
                    print(F"{msgg['msg']}")

        elif msgg['type'] == 'cmd' and msgg['is_json']:
            handle_cmd(msgg['msg'], client)
        else:
            print(F'Unhandled: {msgg}')
    except:
        print(f"RX: {msg!r}\nTopic: {topic!r}\n")
    

def on_disconnect(client, userdata, rc):
    pass

def get_client(on_conn=None, on_msg=None, on_disconn=None):
    # Create a client instance
    client = mqtt.Client(client_id=client_id)  # Replace with your desired client_id

    # Set credentials
    client.username_pw_set(username, password)

    # Assign the callbacks to the client
    client.on_connect = on_connect if not on_conn else on_conn
    client.on_message = on_message if not on_msg else on_msg
    client.on_disconnect = on_disconnect if not on_disconn else on_disconn
    client.reconnect_delay_set(min_delay=1, max_delay=60)

    while True:
        try:
            if use_websockets:
                # Connect to the MQTT broker using WebSockets
                client.ws_set_options(path="/mqtt")
                client.connect(broker_address, ws_port, keepalive=broker_timeout)
                print('connected')
            else:
                # Connect to the MQTT broker using SSL/TLS
                client.tls_set()
                client.connect(broker_address, mqtt_port, keepalive=broker_timeout)
                print('connected')
                break
        except KeyboardInterrupt:
            # Disconnect and stop the network loop when manually interrupted
            client.disconnect()
            print("Disconnected from the MQTT broker")

    return client

async def handle_client(reader, writer):
    try:
        while True:
            data = await reader.read(240)
            if not data:
                break
            
            data = data.decode().strip()
            print(F'client sent {data}')
            if 'name-me' in data:
                conns['telnet'] = [reader, writer]
            else:
                message = MESSAGE
                if 'nitb' in data or 'osmobb' in data:
                    message['type'] = 'cmd'
                    message['is_json'] = True
                else:
                    message['type'] = 'usr'
                message['msg'] = data
                client.publish(pub, json.dumps(message))
    except asyncio.CancelledError:
        pass
    finally:
        writer.close()
        await writer.wait_closed()
        print("Client disconnected.")

async def connect(client):
    res = await loop.run_in_executor(None, client.loop_forever)
    return res

async def local_server():
    server = await asyncio.start_server(handle_client, "localhost", 8888)
    print("WebSocket-like server started and listening on localhost:8888")
    try:
        await server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.close()
        await server.wait_closed()
        print("WebSocket-like server closed.")

async def main():
    global client
    global conns
    conns = {}
    client = get_client()
    tasks = [ local_server(),  connect(client)]
    await asyncio.gather(*tasks)
    

if __name__ == '__main__':
    loop = asyncio.get_event_loop()
    try:
        # Run the main function that includes the async tasks
        loop.run_until_complete(main())
    finally:
        # Close the event loop at the end
        loop.close()
     
    