# WWMI RT Capture 0.1.2 — presupuesto RT independiente

Companion ReShade add-on for developing **full native RT mesh support** in wwmi-dx12.
This is a diagnostic prerequisite, **not an implementation of RT mesh replacement**.
The production add-on still stops when RT is detected. Its safety gate is unchanged.

## Cambio por el fallo de inicio reportado

La 0.1.0 impidió iniciar el juego en el equipo de prueba del usuario. El log
disponible confirma que se registraba el capturador, pero no identifica la
excepción ni demuestra una causa concreta. Esa versión se retira de las pruebas.

El usuario confirmó que la 0.1.1 inicia y capturó datos. La 0.1.2 conserva ese
arranque inactivo, pero sus nuevos hooks todavía deben validarse en el juego:

- `DllMain` solo registra/desregistra el add-on. La preparación se hace en
  `AddonInit`, fuera del bloqueo del cargador.
- Los hooks de recursos, shaders, descriptores, dibujo y RT permanecen inactivos
  hasta pulsar **F7 dentro del juego**. Desactivados no entran en sus callbacks,
  consultan objetos nativos ni toman el mutex de captura.
- El dispositivo DX12 se elige en el primer Present, después de la creación.
- `wwmi_rt_capture_boot.log`, junto al ejecutable, registra el inicio sin
  depender de la opción debug de WWMI. Se vacía en cada nuevo arranque: guárdalo
  antes de repetir una prueba fallida. Incluye marcas antes de activar hooks.
- **F9** desactiva también el seguimiento, además de cancelar la captura.

La 0.1.2 reserva un segundo búfer para BLAS/TLAS y DispatchRays: llenar el
registro de draws ya no termina la captura ni consume el espacio de RT. También
consulta los metadatos de búferes anteriores a F7 al recibir un handle válido en
un callback. No interpreta una dirección GPU absoluta como un objeto nativo.
Los pipelines desconocidos conservan su handle, sin atribuirles otro shader.
Reactivar F7 tras F9 descarta asociaciones antiguas: durante la pausa podrían
haberse destruido o reutilizado sus handles.

## Capturar en el juego

1. Usa ReShade 6.8.0 o posterior, con soporte completo de add-ons, y DirectX 12.
2. Copia `wwmi_rt_capture.addon64` al mismo directorio de add-ons que ya utiliza
   `wwmi_dx12.addon64`. Si ReShade.ini tiene `AddonPath`, usa esa carpeta.
3. Inicia el juego. En `wwmi_rt_capture_boot.log` debe aparecer
   `WWMI RT Capture 0.1.2 AddonInit`.
4. La referencia recibida no contiene eventos RT. La siguiente prueba pendiente
   es con **RT activado** en los ajustes del juego; reinicia si lo solicita.
5. Muestra el mismo personaje en mundo abierto. Pulsa **F7**, cambia al personaje
   de prueba para provocar recursos nuevos, muévelo y pulsa **F8** una vez.
   El add-on original puede desactivar los mods; esta captura es independiente.
6. Espera el mensaje `saved to wwmi_rt_captures` en ReShade.log. Encontrarás
   una carpeta por captura en `Win64/wwmi_rt_captures`, junto al ejecutable.
   Cada carpeta contiene `capture.json` y `shaders/*.dxil`.
7. Comparte la nueva carpeta comprimida e indica que RT estaba activado.
   El mod y la referencia anterior ya se recibieron; no hace falta reenviarlos.

**F9** cancela una captura pendiente y desactiva el seguimiento. Mantén el juego en primer plano para usar
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

La memoria de filas está limitada a 8 MiB/32768 eventos generales y otros
8 MiB/32768 eventos RT, más sus índices/objetos de almacenamiento; los metadatos
tienen un límite de 16 MiB, los shaders de 16 MiB (máximo 2048) y hay hasta
131072 descriptores. Se capturan seis fotogramas o hasta llenar el presupuesto
RT. Llenar el registro general deja incompletos los enlaces generales posteriores,
pero los eventos RT siguen registrándose. `general_trace_truncated` y los
contadores `dropped_*` exponen pérdidas; estos contadores no cuentan callbacks
que ya no entraron por estar lleno su presupuesto. Un fallo de guardado se
comunica en el log.

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

Los búferes anteriores a F7 se identifican de forma diferida cuando un callback
proporciona un handle válido; los shaders anteriores a F7 siguen desconocidos.
Las direcciones RT absolutas se registran sin desreferenciarlas. Los metadatos
faltantes quedan sin resolver. Cambiar de personaje puede crear recursos nuevos, pero no garantiza
la recreación de shaders. Esta limitación impide usar la captura como prueba de
que se hayan identificado todos los enlaces del modelo.

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
python -m unittest discover -s tests -p 'test_rt_*py'
```

El compilado de desarrollo entregado fue compilado para Windows x64 con GCC
MinGW-w64 13-posix y el SDK ReShade `05c95f4a`. Aún no se ha ejecutado dentro del
juego. Compilarlo no valida el comportamiento del driver ni el resultado visual.

## Preparación de geometría

`inspect_mod.py carpeta_mod --output perfil.json` valida tamaños reales, índices,
posiciones y pesos sin editar el mod. Separa los rangos originales usados para
matching de los `drawindexed` reales del mod y conserva sus condiciones como
texto, sin ejecutar la lógica INI. No resuelve automáticamente huesos nativos RT.

`reshade_addon/rt/shaders/SkinModPositions.hlsl` y `skin_reference.py` preparan
el skinning canónico de cuatro u ocho influencias. El kernel se compila por
separado con DXC; **no se ejecuta desde el capturador ni desde el módulo RT**.
Ver el README de shaders para contratos de recursos y límites de validación.

## Correlacionar componentes con BLAS

```sh
python tools/rt_capture/match_rt_geometry.py captura/capture.json perfil.json --output candidatos.json
```

Compara todos los componentes en orden, sus conteos originales y las direcciones
relativas de sus ventanas de índices. Exige posiciones FLOAT3 compartidas y un
mismo búfer base de índices; busca además draws con los mismos VB/IB y rangos.
Devuelve todas las variantes compatibles sin elegir una ni activar reemplazos.
No compara solo el número total de índices. Las coincidencias siguen siendo
candidatos de metadatos CPU: la reutilización de VA, el tiempo de vida, el orden
GPU, la paleta nativa y los materiales requieren verificación independiente.

La captura RT recibida de 0.1.2 contiene 300 eventos BLAS, 3 TLAS y 12
DispatchRays. Los ocho rangos del personaje y el accesorio tienen candidatos
con correlaciones raster. **Esto no habilita el módulo RT:** `rt_stub.cpp`
continúa devolviendo false en `RtModulePresent()` y el add-on bloquea los mods.

Los descriptores de tablas de 0.1.2 pueden haberse perdido por su límite de
capacidad. Además, en la conversión nativa D3D12 del SDK fijado, los offsets y
tamaños de vistas typed/raw se expresan en elementos aunque la estructura API
los documente como bytes. La 0.1.2 los guardó directamente con nombres de bytes.
Sus asociaciones UAV no deben usarse para deducir una paleta de huesos ni un
rango exacto sin normalizar las unidades según formato y versión. El correlador
de geometría usa únicamente las direcciones VB/IB/AS y no depende de esas
muestras de descriptores.
