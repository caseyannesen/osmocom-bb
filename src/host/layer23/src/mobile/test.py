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
osmobb_cfg_dir="/home/debian/osmocom-bb/src/host/layer23/src/mobile/"

MESSAGE = {
    'type':'cmd',
    'msg':'',
    'is_res': False,
    'is_json': False
}

def configure_ms(datas=[]):
    ms_temp_smp = open(F'{osmobb_cfg_dir}default_tmp_ms.cfg').read()
    base_temp = open(F'{osmobb_cfg_dir}default_tmp_base.cfg').read()
    for index, data in enumerate(datas):
        if index > 5:
            break
            
        if len(data) >= 15:
            imsi = data['imsi']
        else:
            imsi = input('Enter imsi: ')
        
        if ms_temp_smp:
            ms_temp = ms_temp_smp.replace('{{index}}', str(index+1))
            ms_temp = ms_temp.replace('{{imsi}}', imsi)
            ms_temp = ms_temp.replace('{{imei}}', ''.join(random.sample('0123456789' * 100, 4)))
            ms_temp = ms_temp.replace('{{mcc}}', imsi[:3])
            ms_temp = ms_temp.replace('{{mnc}}', imsi[3:5])
            print(ms_temp)
            base_temp = base_temp.replace('!{{mobile_' + F'{index}' + '}}', ms_temp)
            out = open(F'{osmobb_cfg_dir}default.cfg', 'w')
            out.write(base_temp)

if __name__ == '__main__':
    configure_ms(['1','2','3','4'])
