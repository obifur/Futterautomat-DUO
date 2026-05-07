import serial
from datetime import datetime
from itertools import product

port = "/dev/ttyUSB0"
baud = 9600
response_timeout = 0.1

length = 8  # 16 Byte Array

# >>> HIER STARTPOSITION UND STARTWERT EINSTELLEN <<<
start_pos = 0       # 0–15
start_value = 0x00  # 0x00–0xFF

# Werte, die die anderen Bytes annehmen dürfen
allowed = [0x00, 0x0F, 0x0B, 0xFA, 0xFF]

log_path = "bruteforce_hex.log"

print(f"Öffne {port} mit {baud} Baud...")
print(f"Brute force: 16-Byte-Befehle")
print(f"Nur Position {start_pos} läuft 00–FF, alle anderen nutzen {allowed}")
print(f"Startwert: {start_value:02X}")

with serial.Serial(port, baud, timeout=response_timeout) as ser, open(log_path, "a", encoding="utf-8") as log_file:
    try:
        for pos in range(start_pos, length):

            # Startwert nur für die erste Position
            value_start = start_value if pos == start_pos else 0x00

            # Alle anderen Positionen bekommen Werte aus allowed
            other_positions = [i for i in range(length) if i != pos]

            # Erzeuge alle Kombinationen für die anderen 15 Bytes
            for combo in product(allowed, repeat=len(other_positions)):

                # Mapping der festen Werte auf die Positionen
                base = bytearray([0x00] * length)
                for idx, p in enumerate(other_positions):
                    base[p] = combo[idx]

                # Jetzt die eigentliche Bruteforce-Position durchlaufen
                for value in range(value_start, 0x100):

                    base[pos] = value
                    cmd_bytes = bytes(base)

                    ser.reset_input_buffer()
                    ser.reset_output_buffer()
                    ser.write(cmd_bytes)
                    ser.flush()

                    data = ser.read(1024)
                    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                    hex_cmd = cmd_bytes.hex(" ").upper()

                    if data:
                        hex_line = data.hex(" ").upper()
                        entry = f"{timestamp}  Pos={pos}  Wert={value:02X}  Befehl={hex_cmd}  Antwort={hex_line}\n"
                        log_file.write(entry)
                        log_file.flush()
                        print(entry, end="")
                        print(f"Antwort entdeckt bei Position {pos}, Wert {value:02X}. Stop.")
                        raise SystemExit
                    else:
                        entry = f"{timestamp}  Pos={pos}  Wert={value:02X}  Befehl={hex_cmd}  Antwort=<keine>\n"
                        log_file.write(entry)
                        log_file.flush()
                        print(entry, end="")

            # Nach der ersten Position immer bei 0x00 starten
            value_start = 0x00

        print("Keine Antwort für alle Befehle gefunden.")

    except KeyboardInterrupt:
        print("Abgebrochen durch Benutzer.")
    except serial.SerialException as e:
        print(f"SerialException: {e}")
