# WWMI RT Capture 0.1

Companion ReShade add-on for developing **full native RT mesh support** in wwmi-dx12.
This is a diagnostic prerequisite, **not an implementation of RT mesh replacement**.
The production add-on still stops when RT is detected. Its safety gate is unchanged.

## Capturar en el juego

1. Usa ReShade 6.8.0 o posterior, con soporte completo de add-ons, y DirectX 12.
2. Copia `wwmi_rt_capture.addon64` al mismo directorio de add-ons que ya utiliza
   `wwmi_dx12.addon64`. Si ReShade.ini tiene `AddonPath`, usa esa carpeta.
3. Inicia el juego. En ReShade.log debe aparecer `WWMI RT Capture 0.1`.
4. Con RT **desactivado**, muestra un solo personaje cuyo mod funcione en DX12.
   Mueve al personaje y pulsa **F8** una vez. Se capturan seis fotogramas.
5. Activa RT y reinicia si el juego lo solicita. Muestra el mismo personaje,
   muévelo y vuelve a pulsar **F8**. El add-on original puede desactivar los mods;
   esta captura independiente sigue funcionando.
6. Espera el mensaje `saved to wwmi_rt_captures` en ReShade.log. Encontrarás
   una carpeta por captura en `Win64/wwmi_rt_captures`, junto al ejecutable.
   Cada carpeta contiene `capture.json` y `shaders/*.dxil`.
7. Comparte ambas carpetas comprimidas, el mod de prueba y su nombre de personaje.
   Conserva el .ini y los .buf del mod: hacen falta para contrastar los índices
   de huesos y los componentes con la captura.

**F9** cancela una captura pendiente. Mantén el juego en primer plano para usar
las teclas. Se escriben archivos solamente al terminar la captura; puede haber
una pausa puntual al guardar. No se capturan continuamente ni se envían datos.
Para retirar la herramienta, cierra el juego y elimina `wwmi_rt_capture.addon64`.
Puedes conservar o borrar las carpetas de capturas por separado.

## Qué registra

- BLAS/TLAS, geometrías, VA originales, offsets, formatos, número de vértices e
  índices, flags, modo build/update, destino y scratch.
- Raíces de cómputo y gráficos, constantes, layouts, shaders y búferes de dibujo.
- Muestras de descriptores de búfer en tablas de cómputo: hasta los primeros 64
  slots por tabla y un máximo de 64 búferes por dispatch.
- DispatchRays y los rangos/strides de sus cuatro tablas SBT.
- Identidad y generación de listas, cola de envío, secuencia CPU y tiempos QPC.
- Inventario de recursos y shaders con identificadores independientes de las
  direcciones, para distinguir la reutilización de handles.

No cambia draw calls, shaders, vértices, BLAS, TLAS ni tablas del juego. Todos los
callbacks con retorno booleano devuelven false. No hace readback GPU, ni espera
fences, ni llama Map sobre buffers del juego. Los shaders retenidos se copian
del bytecode CPU recibido en los eventos de creación de pipeline.

La memoria está limitada: 8 MiB/32768 eventos, 16 MiB de metadatos, 16 MiB de
shaders (máximo 2048) y 131072 descriptores. La captura termina anticipadamente si se llena
el búfer de eventos. Los contadores `dropped_*` exponen las pérdidas; una captura
truncada no se presenta como completa. Un fallo de guardado se comunica en el log.

## Análisis offline

Con Python 3.10 o posterior:

```sh
python tools/rt_capture/analyze_capture.py ruta/capture.json
```

Crea `analysis.json` junto a la captura. Busca coincidencias de direcciones entre
UAV de cómputo y posiciones FLOAT3 usadas para construir BLAS. Son **candidatos**:
no prueban que un shader escriba ese rango ni que pertenezca al personaje.
Las tablas pueden contener descriptores no utilizados por el shader. El orden
de callbacks CPU tampoco demuestra el orden GPU entre listas distintas.

La captura no contiene los vértices, matrices de huesos, instancias TLAS ni
registros locales SBT almacenados en GPU. Hace falta una fase posterior de
readback con fences para verificar esos contenidos. La destrucción de heaps
de descriptores no se observa mediante esta API; sus asociaciones se tratan
como candidatos. Las vinculaciones anteriores a F8 quedan desconocidas.

## Lo que falta para modelos completos

1. Verificar en una captura del juego qué dispatch genera el skin cache de cada
   personaje, el formato de la paleta de huesos y el orden GPU.
2. Generar las posiciones animadas del mod, incluidos remapeos de huesos,
   shapekeys y copias independientes del personaje, usando esos datos verificados.
3. Construir BLAS para la topología del mod y redirigir las instancias TLAS.
   Preservar el índice de geometría/material no basta cuando cambia el número
   de segmentos: hay que adaptar también atributos y registros locales SBT.
4. Mantener recursos/scratch por envío hasta su fence; cubrir actualizaciones,
   compactación, cambio de personaje/LOD, recarga y vuelta al modelo original.
5. Validar en el juego sombras/reflejos, materiales translúcidos, animación,
   dos copias del personaje y frame generation, con el debug layer de DX12.

Quitar `RtBlockWithoutModule()` o devolver true desde `RtModulePresent()` sin
esas conexiones solo elimina el bloqueo: no implementa modelos completos en RT.

## Compilar

En la raíz del repositorio, con Visual Studio 2022 (C++ desktop + Windows SDK):

```bat
tools\rt_capture\build.cmd
```

Salida: `build_addon/rt_capture/wwmi_rt_capture.addon64`. Solo necesita los headers
del SDK ReShade fijado por el submódulo; no usa 3Dmigoto, ImGui ni DXC.
El workflow `rt-capture.yml` compila y prueba esta herramienta sin publicar releases.

Pruebas de lógica CPU:

```sh
g++ -std=c++20 -Wall -Wextra -Werror tests/rt_capture_test.cpp -o /tmp/rt_capture_test
/tmp/rt_capture_test
python -m unittest discover -s tests -p 'test_rt_capture_analysis.py'
```

El compilado de desarrollo entregado fue compilado para Windows x64 con GCC
MinGW-w64 13-posix y el SDK ReShade `05c95f4a`. Aún no se ha ejecutado dentro del
juego. Compilarlo no valida el comportamiento del driver ni el resultado visual.
