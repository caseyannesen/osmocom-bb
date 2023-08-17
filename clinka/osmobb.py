import random
import telnetlib
import json

osmobb_cfg_dir="../src/host/layer23/src/mobile/"

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