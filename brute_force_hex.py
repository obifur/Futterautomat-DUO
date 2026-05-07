import serial
from datetime import datetime

port = "/dev/ttyUSB0"
baud = 9600  # Passe die Baudrate ggf. an
response_timeout = 0.5  # Sekunden nach jedem gesendeten Befehl warten
start_byte = 0x00
end_byte = 0xFF
log_path = "bruteforce_hex.log"

print(f"Öffne {port} mit {baud} Baud...")
print(f"Brute force: Sende HEX-Befehle von {start_byte:02X} bis {end_byte:02X}")
print(f"Antwort wartet {response_timeout} Sekunden pro Befehl")

with serial.Serial(port, baud, timeout=response_timeout) as ser, open(log_path, "a", encoding="utf-8") as log_file:
    try:
        for value in range(start_byte, end_byte + 1):
            cmd = bytes([value])
            ser.reset_input_buffer()
            ser.reset_output_buffer()
            ser.write(cmd)
            ser.flush()

            data = ser.read(1024)
            timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            if data:
                hex_line = data.hex(" ").upper()
                entry = f"{timestamp}  Befehl={value:02X}  Antwort={hex_line}\n"
                log_file.write(entry)
                log_file.flush()
                print(entry, end="")
                print(f"Antwort entdeckt bei Befehl {value:02X}. Beende Brute Force.")
                break
            else:
                entry = f"{timestamp}  Befehl={value:02X}  Antwort=<keine>\n"
                log_file.write(entry)
                log_file.flush()
                print(entry, end="")
        else:
            print("Keine Antwort für alle Befehle gefunden.")
    except KeyboardInterrupt:
        print("Abgebrochen durch Benutzer.")
    except serial.SerialException as e:
        print(f"SerialException: {e}")
