# vtOS: Un firmware hobby basado en terminal

Este proyecto cuenta con soporte de primera clase para el [LILYGO T-Deck](https://s.click.aliexpress.com/e/_c4agv9Wd), transformándolo en una terminal portátil independiente.

Este proyecto implementa un emulador de terminal de alto rendimiento y consciente de los atributos para MicroPython. Al envolver el motor de [st](https://st.suckless.org/) (suckless terminal) en un módulo C personalizado, logra funciones de terminal de clase escritorio en hardware embebido, incluyendo una **barra de estado de asignación cero** para telemetría en tiempo real sin fragmentación del heap.

Como muestra de las capacidades del motor, este proyecto incluye un puerto en C totalmente funcional y consciente de VFS de [neatvi](https://github.com/aligrudi/neatvi), un **editor de texto vi/ex**, y el **intérprete ZMachine** [frotz](https://davidgriffith.gitlab.io/frotz/) que permite jugar juegos de texto clásicos como [Zork](https://en.wikipedia.org/wiki/Zork). El firmware proporciona un **cliente Telnet**, un **servidor FTP**, un **Administrador de Archivos** basado en TUI, un **Cliente IRC**, un **Lector de RSS** y un **Administrador de Red** basados en Python, demostrando cómo se puede extender fácilmente el motor de la terminal.

| Demo ASCII (ejecutándose en CYD) | App vi |
| :---: | :---: |
| <img src="assets/screen.gif" alt="ascii demo" width="400"> | <img src="assets/screen2.jpg" alt="vi app" width="400"> |

| Buscamínas (telnet) | Zork (telnet) |
| :---: | :---: |
| <img src="assets/screen3.jpg" alt="minesweeper" width="400"> | <img src="assets/screen4.jpg" alt="zork" width="400"> |


## 📟 Integración de Hardware T-Deck

Este proyecto está optimizado para el **LilyGO T-Deck**, aprovechando MicroPython para interactuar con el ESP32-S3 y sus periféricos integrados.

### 🛠 Componentes Soportados

| Componente | Especificación | Controlador / Estado |
| :--- | :--- | :--- |
| **Memoria** | 8MB PSRAM / 16MB Flash | ✅ Habilitado para manejo de buffers grandes |
| **Pantalla** | LCD ST7789 de 2.4" (320x240) | ✅ Bus SPI optimizado (Color Completo) |
| **Radio LoRa** | SX1262 | ✅ Utilidad de configuración propia |
| **Teclado** | Teclado LILYGO | ✅ Interfaz I2C mapeada |
| **Trackball** | Trackball LILYGO | ✅ Interfaz I2C mapeada |
| **Altavoz** | I2S | ✅ Soporte de reproducción MP3/WAV |
| **Tarjeta SD** | SPI | ✅ Tarjeta SD formateada en FAT |
| **Micrófono** | I2S, ADC ES7210 | ❌ Problemas de ruido |
| **Pantalla Táctil** | GT911 | N/A |


## ⚡ Instalación Rápida (Binarios Pre-compilados)

Puedes descargar y flashear el firmware pre-compilado más reciente directamente en tu T-Deck.

### Opción A: Flashear desde la web

1. **Descargar el Firmware**: Ve a la [Página de Releases](https://github.com/8bitmcu/vtOS/releases) y descarga el último archivo *.bin.

2. **Flashear al T-Deck**: Utiliza una herramienta de flasheo web como [https://web.esphome.io](https://web.esphome.io) para flashear el binario.

### Opción B: Flashear desde línea de comandos

1. **Descargar el Firmware**: Ve a la [Página de Releases](https://github.com/8bitmcu/vtOS/releases) y descarga el último archivo *.bin.

2. **Flashear al T-Deck**: Asegúrate de que tu T-Deck esté conectado vía USB, luego usa esptool.py para escribir el firmware en el dispositivo. Es posible que necesites instalar esptool primero (`pip install esptool`).

```Bash

esptool.py -p /dev/ttyACM0 -b 460800 --chip esp32s3 write_flash 0x0 firmware.bin

```

### Opción C: Bootloader (Launcher)

Este firmware es totalmente compatible con [Launcher](https://bmorcelli.github.io/Launcher), un lanzador de aplicaciones y bootloader integrado en el dispositivo para dispositivos ESP32. Este método es perfecto si quieres cambiar fluidamente entre `vtOS` y otros firmwares sobre la marcha sin necesidad de un PC.

1. **Instalar Launcher**: Abre un navegador compatible con Web Serial (como Chrome o Edge) y navega al sitio web de Launcher. Selecciona Web Flasher y sigue las instrucciones para instalar el bootloader directamente en tu T-Deck.
2. **Preparar tu Tarjeta SD**: Descarga el último *.bin desde la Página de Releases y cópialo en una tarjeta MicroSD.
3. **Arrancar e Instalar**: Inserta la tarjeta MicroSD en tu T-Deck y enciéndelo. Usando la interfaz de Launcher en la pantalla del dispositivo, navega hasta tu tarjeta SD y selecciona el binario de `vtOS` para flashearlo.


## 📝 Cómo usar

### Uso del Trackball
- **Presión corta**: Envía `Esc`
- **Presión larga**: Envía `KeyboardInterrupt`
- **Desplazar arriba**: Desplazar hacia arriba el historial de la terminal st
- **Desplazar abajo**: Desplazar hacia abajo el historial de la terminal st
- **Desplazar izquierda**: Desplazar hacia arriba el historial de comandos de st
- **Desplazar derecha**: Desplazar hacia abajo el historial de comandos de st

### Comandos disponibles

Puedes ejecutar los siguientes comandos desde la shell integrada:

| Comando | Descripción |
| :---   | :--- |
| `chess` | Juego de ajedrez básico usando caracteres unicode |
| `clear` | Limpia la pantalla |
| `c2` | Utilidad de codificación/decodificación de audio/voz Codec 2 |
| `dict` | Cliente de búsqueda de diccionario mediante protocolo DICT |
| `echo` | Imprime sus argumentos |
| `fav` | Alias de la shell integrados |
| `fc` | Utilidad de Configuración de Fuentes. Prueba `menu fc` |
| `fm` | Inicia el Administrador de Archivos TUI |
| `ftp` | Cliente FTP que monta su contenido como un VFS |
| `ftpd` | Inicia un Servidor FTP en `/` con usuario `admin` y contraseña `admin`. Sin cifrar. |
| `gemini` | Navegador del protocolo Gemini; conéctate a una url `gemini://`, o usa `menu gemini` para sitios guardados |
| `gopher` | Navegador del protocolo Gopher; conéctate a una url `gopher://`, o usa `menu gopher` para sitios guardados |
| `irc` | Se conecta a un canal IRC dado un servidor, puerto, apodo y canal. Prueba `menu irc` |
| `loracfg` | Utilidad para configurar la frecuencia y potencia de LoRa. Prueba `menu loracfg` |
| `lorachat` | Un chat básico basado en la radio LoRa |
| `menu` | Un menú interactivo de accesos directos para comandos. |
| `mines` | Abre el clon de buscamínas |
| `nm` | Administrador de Red TUI. También puede usarse como herramienta de línea de comandos |
| `ping` | Se usa para probar la alcanzabilidad de un host determinado |
| `play` | Reproductor de audio que soporta archivos WAV, MP3 y codificados en C2 (Codec 2). |
| `pop3` | Cliente de correo electrónico POP3; conecta con `pop3 <host> <usuario> <contraseña> [puerto]` |
| `rec` | Grabador de audio que graba en WAV o C2 (Codec 2). Nota: Problemas de ruido actualmente no resueltos |
| `rss` | Lector de RSS; conéctate a una url de RSS para recuperar los artículos, o usa `menu rss` para feeds guardados |
| `sftp` | Cliente SFTP que monta su contenido como un VFS |
| `sftpd` | Servidor SFTP; se ejecuta en segundo plano |
| `smtp` | Cliente de correo electrónico SMTP; redacta en `vi` y envía con `smtp <host> <usuario> <contraseña> [puerto]` |
| `ssh` | Cliente SSH; conéctate a un servidor ssh remoto |
| `sshd` | Servidor SSH; se ejecuta en segundo plano, comparte la shell de este dispositivo. |
| `stream` | Transmite radio por internet (MP3 sobre HTTP); pasa una URL, o usa `menu stream` para estaciones guardadas |
| `telnet` | Se conecta a un servidor telnet. prueba `menu telnet` |
| `telnetd` | Servidor Telnet; se ejecuta en segundo plano, comparte la shell de este dispositivo. Sin cifrar. |
| `usbmsc` | Comparte `/sd` con tu PC como una unidad de almacenamiento masivo USB a través de USB-C. |
| `vi` | Abre el puerto de vi (basado en [neatvi](https://github.com/aligrudi/neatvi)). Incluye 5 temas configurables, revisa `.virc` |
| `vncd` | Inicia un Servidor VNC. Compatible con TigerVNC. Terriblemente lento |
| `webvncd` | Inicia un servidor web similar a "VNC". Más rápido que `vncd` |
| `wiki` | Lector offline de Wikipedia en inglés simple; ~170MB desde SD. Ver `utils/wikiconvert.py` |
| `zm` | Inicia `dfrotz`, el intérprete ZMachine |

Para salir de la shell, escribe `exit`. Esto te llevará a la shell de MicroPython, donde puedes escribir expresiones de python. Para volver a la shell integrada, escribe `sh` en la shell de MicroPython.

### Script de inicio (`.shellrc`)

Al iniciar, la shell ejecuta comandos de un archivo `.shellrc`, si existe. Verifica primero `/flash/.shellrc`; si no existe, recurre a `/sd/.shellrc` (solo uno de los dos se ejecuta, nunca ambos). Cada línea se trata exactamente como algo que escribirías en el prompt de la shell, incluyendo argumentos entre comillas; las líneas en blanco y las que comienzan con `#` se ignoran. Terminar el archivo con `exit` omite el prompt interactivo por completo y te lleva directamente al REPL de MicroPython después de que se ejecute el resto del archivo.

Ejemplo `/flash/.shellrc`:
```
# fuente predeterminada
fc terminus_mpy_12

# conectarse a wifi
nm connect MiRed "mi contraseña de wifi"

# lanza el menú interactivo
menu
```

## 🔨 Cómo Compilar (T-Deck)

### Opción A: Compilar en la máquina anfitriona

Compilar este proyecto requiere un compilador cruzado para el ESP32-S3 y el árbol de fuentes de MicroPython. Asegúrate de tener instalado el ESP-IDF (Espressif IoT Development Framework). Este proyecto está verificado usando **MicroPython v1.28.0** y **ESP-IDF v5.5.1**.

```bash
# Clonar este repositorio
git clone https://github.com/8bitmcu/vtOS.git

# Copiar la definición de placa T-Deck en tu directorio de fuentes de micropython:
cp -r /ruta/a/vtOS/boards/LILYGO_T_DECK /ruta/a/micropython/ports/esp32/boards/

# Copiar idf_component.yml en los puertos esp32 de micropython:
cp /ruta/a/vtOS/modules/idf_component.yml /ruta/a/micropython/ports/esp32/main/

# Inicializar el entorno ESP-IDF
source $HOME/esp/esp-idf/export.sh

# Compilar el compilador cruzado de MicroPython
make -C /ruta/a/micropython/mpy-cross

# Navegar al directorio del puerto T-Deck.
cd /ruta/a/micropython/ports/esp32

# Especificar el BOARD como LILYGO_T_DECK para habilitar el soporte de PSRAM / Flash
# Especificar USER_C_MODULES y FROZEN_MANIFEST para módulos C y scripts de python
make BOARD=LILYGO_T_DECK USER_C_MODULES=/ruta/a/vtOS/modules FROZEN_MANIFEST=/ruta/a/vtOS/modules/manifest.py

# Flashear el firmware al dispositivo
esptool.py -p /dev/ttyACM0 -b 460800 --chip esp32s3 write_flash 0x0 firmware.bin
```

### Opción B: Compilar usando Docker y Makefile

No necesitas instalar el ESP-IDF, toolchains o el código fuente de MicroPython en tu máquina anfitriona si tienes `Docker` y `Make` instalados.

#### Prrequisitos:

- Docker instalado y ejecutándose.
- Make instalado en tu sistema anfitrión.
- Un entorno Linux (o WSL2 en Windows) que permita el paso de dispositivos USB a Docker.

Si solo quieres compilar y flashear el firmware, ejecuta estos comandos en orden:


```Bash

# 1. Clonar este repositorio y entrar en él
git clone https://github.com/8bitmcu/vtOS.git && cd vtOS

# 2. Inicializar el entorno (descarga las fuentes de MicroPython)
make init

# 3. Compilar el firmware
make build

# 4. Flashear al dispositivo (asegúrate de que tu T-Deck esté conectado)
make flash
```
#### Referencia del Makefile

`make init` configura el entorno de construcción limpio. Construye la imagen de Docker local necesaria (micropython-build) y crea un volumen de Docker persistente. Luego clona MicroPython y sus submódulos directamente en ese volumen. Ejecuta esto una vez al configurar el proyecto, o para forzar una descarga fresca de las fuentes de MicroPython.

`make build` compila el firmware de MicroPython. Monta tus directorios locales boards/ y modules/ en el contenedor ESP-IDF. Compila los módulos a nivel de C, congela tus manifiestos de Python y construye el objetivo específicamente para el LILYGO_T_DECK. Salida: Los binarios compilados (firmware.bin, micropython.bin) aparecerán en tu carpeta local `build_output/`.

`make flash` flashea el firmware compilado al ESP32-S3. Usa `esptool.py` dentro del contenedor para borrar la flash y escribir el nuevo firmware.bin en la dirección 0x0.

`make sync_files` transfiere tu código de aplicación Python al dispositivo. Usa `mpremote` para copiar recursivamente todo dentro de ./modules/scripts/ a la raíz del sistema de archivos flash interno del T-Deck.

`make sync_file FILE=nombrearchivo.py` transfiere un solo archivo de script Python al dispositivo. Usa `mpremote` para copiar desde ./modules/scripts/ a la raíz del sistema de archivos flash interno del T-Deck.

`make repl` abre el prompt interactivo de MicroPython. Conecta tu terminal a la salida serial del dispositivo vía mpremote. Presiona Ctrl+D dentro del REPL para activar un reinicio suave, o Ctrl+] para salir y volver a la terminal del anfitrión.

`make core_dump` analiza fallos fatales. Si tu dispositivo falla y entra en un bucle de reinicio o se detiene, este comando lee la partición de coredump raw directamente de la flash y la mapea contra el archivo .elf en tu volumen de construcción para proporcionar un stack trace a nivel de C legible para humanos.

`make clean` limpia los artefactos de construcción. Elimina los archivos objeto compilados del volumen de Docker y borra la carpeta local build_output/ para asegurar que tu próximo make build comience completamente desde cero.

#### Sobrescribir Variables

El Makefile usa por defecto /dev/ttyACM0 para la conexión USB. Si tu SO asigna un puerto diferente (ej. /dev/ttyUSB0), puedes sobrescribirlo en línea sin editar el Makefile:

```Bash
make flash PORT=/dev/ttyUSB0
make repl PORT=/dev/ttyUSB0
```


## ⚖️ Licencia y Atribución

El código fuente de este proyecto está bajo la **Licencia MIT**. Sin embargo, si compilas el firmware con el módulo opcional `frotz` o la fuente de iconos `siji` habilitados, el binario compilado resultante se distribuye bajo la **Licencia GPLv2**.

### Componentes de Terceros:
* **Licencia st:** MIT (c) ingenieros de st.
* **st7789_mpy:** (c) Russ Hughes. Licencia MIT
* **vi** (neatvi): (c) Ali Gholami Rudi. Licencia ISC.
* **frotz**: (c) Stefan Jokisch, David Griffith. Licencia GPLv2
* **yxml**: Copyright (c) 2013-2014 Yoran Heling. Licencia MIT
* **codec2**: (c) 2010 David Rowe. Licencia LGPL-2.1 (incluido íntegramente en `modules/codec2/vendor/`, ver `modules/codec2/COPYING`)
* **MicroPython**: (c) Damien P. George. Licencia MIT

### Fuentes y Activos:
* **Fuente Terminus:** (c) 2020 Dimitar Zhekov. Licenciada bajo la [SIL Open Font License 1.1](https://scripts.sil.org/OFL).
* **Fuente Cozette:** Copyright (c) 2020 Samhain <samhain@moonwit.ch> & colaboradores <https://github.com/the-moonwitch/Cozette/contributors>. Distribuida bajo los términos de la Licencia MIT.
* **Fuente Tamzen:** Copyright 2011 Suraj N. Kurapati <https://github.com/sunaku/tamzen-font>. La fuente Tamzen es gratuita. Por la presente se le concede permiso para usarla, copiarla, modificarla y distribuirla según considere oportuno. La fuente Tamzen se proporciona "tal cual", sin ninguna garantía expresa o implícita. El autor no hace declaraciones sobre la idoneidad de esta fuente para un propósito particular. En ningún caso el autor será responsable de los daños derivados del uso de esta fuente.
* **Fuente Gohu:** Copyright 2015 por Hugo Chargois. Distribuida bajo los términos de la [WTFPL versión 2](https://www.wtfpl.net/about/).
* **Fuente Spleen:** Copyright (c) 2018-2026, Frédéric Cambus. Licenciada bajo la [BSD 2-Clause License](https://github.com/fcambus/spleen/blob/master/LICENSE).
* **Fuente Scientifica:** Copyright (c) 2020 Akshay Oppiliappan <nerdy@peppe.rs>. Licenciada bajo la [SIL Open Font License 1.1](https://scripts.sil.org/OFL).
* **GNU Unifont:** Copyright Roman Czyborra, Paul Hardy y colaboradores. Doble licencia bajo la SIL Open Font License 1.1 y GNU GPL v2+ con la Excepción de Incrustación de Fuentes GNU; utilizada aquí bajo la [SIL Open Font License 1.1](https://scripts.sil.org/OFL).
* **Fuente Siji:** (c) stark y colaboradores. Basada en Stlarch, con glifos dibujados de FontAwesome y otros paquetes de iconos. [Licencia GPLv2](https://github.com/stark/siji)
