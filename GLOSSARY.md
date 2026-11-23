# Nibemote

Vocabulary for the Nibe internal bus as this project works with it: a device that
impersonates the panel, on a capture path that runs whether or not it is impersonating.

## Language

**Master**:
The control unit that owns the bus and drives every exchange by polling nodes.
_Avoid_: host, controller, server

**Panel**:
The display unit on the bus that this project impersonates.
_Avoid_: display card, HMI, handset

**Emulator**:
The mode in which this project answers polls and transmits as the panel.
_Avoid_: active mode, master mode

**Sniffer**:
The capture-only mode, in which the bus is recorded and nothing is transmitted. It runs
whether or not the emulator is on.
_Avoid_: passive mode, tap, monitor

**Exchange**:
One poll and everything the bus does in response, until the bus falls silent again.
_Avoid_: transaction, conversation, session (a menu session is a different thing)

**Poll**:
The master's invitation to one node to speak.
_Avoid_: invitation, prompt

**Grant**:
The master's acknowledgement that a node may send the frame it announced.
_Avoid_: permission, turn

**Frame**:
The addressed, length-tagged, checksummed unit of data a node sends inside an exchange.
_Avoid_: packet, message

**Query**:
A command the panel sends to the master, with the payload it carries and the rule for
sending it again.
_Avoid_: probe, request (the content request is one query among others), command
(the command byte is part of a query)

**Answer**:
The frame a query is met with, when it is met with one. Not every query is answered.
_Avoid_: reply (the panel's reply to a poll is a different thing), response

**Outcome**:
How a query ends: an answer, a NAK, a refusal, or silence.
_Avoid_: result, status

**NAK**:
The control byte a receiver sends when it made out a frame but its checksum was wrong.
_Avoid_: error, negative acknowledgement

**Refusal**:
An ETX from the master in place of an answer: the frame was valid, the command was not
understood.
_Avoid_: error, rejection

**Our slot**:
The exchanges in which the master polls the panel's address.
_Avoid_: display slot, our turn
