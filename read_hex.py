import serial
from datetime import datetime

port = "/dev/ttyUSB0"
baud = 9600  # Passe die Baudrate ggf. an
log_path = "read_hex.log"

print(f"Öffne {port} mit {baud} Baud...")
print(f"Schreibe empfangene Daten zusätzlich in {log_path}")

with serial.Serial(port, baud, timeout=1) as ser, open(log_path, "a", encoding="utf-8") as log_file:
    try:
        while True:
            data = ser.read(16)
            if data:
                hex_line = data.hex(" ").upper()
                timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                entry = f"{timestamp}  {hex_line}\n"
                log_file.write(entry)
                log_file.flush()
                print(entry, end="")
    except KeyboardInterrupt:
        print("Beendet.")
