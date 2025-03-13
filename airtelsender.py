#!/usr/bin/env python3

'''
    This software is written as a skeleton template to run and manage a remote CHEAPRAY network.
'''

import argparse
import subprocess, os
import time, random


def generate_message(details):
   mydate = time.localtime()
   mymon = mydate.tm_mon if len(str(mydate.tm_mon)) != 1 else F"0{mydate.tm_mon}"
   myday = mydate.tm_mday if len(str(mydate.tm_mday)) != 1 else F"0{mydate.tm_mday}"
   myhour = mydate.tm_hour if len(str(mydate.tm_hour)) != 1 else F"0{mydate.tm_hour}"
   mymin = mydate.tm_min if len(str(mydate.tm_min)) != 1 else F"0{mydate.tm_min}"
   TXN_ID = F"CI{str(mydate.tm_year)[2:]}{mymon}{myday}.{myhour}{mymin}.M{random.randint(1000,9999)}"
   message = F"./sms_smpp.py AirtelMoney {details.to} \"You have received ZMW {details.amount} from {details.number} {details.name}. Balance ZMW {round(float(details.agent_balance) + float(details.amount), 2)} commission ZMW {details.commission}. Txn. ID: {TXN_ID}\""
   return message


def main(details):
    name = details.name if details.name else input("Enter name of sender: ")
    number = details.number if details.number else input("Enter number of sender: ")
    amount = details.amount if details.amount else input("Enter amount withdraw: ")
    balance = details.balance if details.balance else input("Enter balance of agent: ")



if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Run CHEAPRAY network manager')
    parser.add_argument('--number', type=str, default='', help='the withdrawing user')
    parser.add_argument('--name', type=str, default='', help='the withdrawing user\'s name')
    parser.add_argument('--amount', type=str, default='', help='the withdrawing amount')
    parser.add_argument('--agent_balance', type=str, default='', help='the agent\'s balance')
    parser.add_argument('--commission', type=str, default='', help='the commission')
    parser.add_argument('--to', type=str, default='', help='the receiver id')
    args = parser.parse_args()

    print(generate_message(args))






