# WWMI RT Capture 0.1.3 — constantes del personaje

Herramienta independiente para desarrollar modelos completos en RT nativo.
**Todavía no implementa reemplazo RT.** El add-on original conserva su bloqueo:
`RtModulePresent()` sigue devolviendo false. No se instala un módulo RT funcional.

## Qué cambia

La captura recibida de 0.1.2 permitió correlacionar las ocho geometrías originales
con los mismos VB/IB usados por los dibujos raster, pero guardaba direcciones,
no bytes de constantes. La 0.1.3 añade copias acotadas UPLOAD → READBACK para
inspeccionarlas, con una firma de geometría configurable y fences propios.
La firma indica un candidato; no verifica identidad, huesos ni materiales.

- Reconoce todos los componentes en orden, sus conteos, ventanas relativas de
  índices, formato FLOAT3, stride 12 y número de vértices nativos.
- Correlaciona draws con VB/IB de ese BLAS; también muestrea dispatches cuyas
  tablas incluyen su dirección de posiciones. Una tabla no demuestra uso real.
- Mantiene el estado de bindings durante los doce Present de la captura aunque
  se llene el presupuesto de filas generales. Los eventos objetivo usan el
  presupuesto RT independiente.
- Corrige las unidades de las vistas typed/raw del backend D3D12 del SDK fijado:
  FirstElement y NumElements se convierten a bytes según el formato, con controles
  de overflow. Las vistas estructuradas y CBV conservan sus unidades propias.
- El analizador normaliza las muestras typed/raw antiguas de 0.1.0–0.1.2. Los
  formatos desconocidos quedan sin resolver.

## Prueba en el juego

1. Con el juego cerrado, copia `wwmi_rt_capture.addon64` y `wwmi_rt_target.txt`
   juntos a tu carpeta de add-ons. Si usas AddonPath, ambos van allí.
2. La configuración incluida deriva del cuerpo Hsin observado: 87374 vértices
   nativos y ocho ventanas originales. No son los 137779 vértices del mod.
3. Activa RT y reinicia si el juego lo solicita. El mod seguirá bloqueado por el
   add-on original; esta herramienta obtiene datos del modelo nativo.
4. En mundo abierto, pulsa F7, cambia al mismo personaje y muévelo. Pulsa F8 una
   vez. Se registran doce Present; después se consultan los fences sin bloquear.
5. Espera `saved to wwmi_rt_captures` en ReShade.log. Comprime la nueva carpeta
   dentro de `Client/Binaries/Win64/wwmi_rt_captures` y compártela completa.
   Puede incluir `upload_windows/*.bin`, además de capture.json y shaders/*.dxil.
6. Si no hay ventanas, conserva la carpeta igualmente: los contadores explican
   si faltó la firma, los enlaces, recursos UPLOAD o una copia completada.

La prueba anterior ya cumplió su objetivo. Esta nueva captura agrega contenido
que antes no existía; no hace falta repetir la referencia con RT apagado ni enviar
el mod otra vez. Requiere ReShade 6.8.0 con soporte completo de add-ons y DX12.

F9 cancela y desactiva el seguimiento. Las copias ya enviadas conservan recursos
hasta su fence; no se cancelan operaciones GPU que ya están en la cola. No hay
captura continua ni envío de datos. Para retirar la herramienta, cierra el juego
y elimina los dos archivos instalados. Conserva los logs si falla el inicio.

## Contrato de las copias

Los callbacks permanecen inactivos hasta F7. DllMain solo registra/desregistra el
add-on; AddonInit prepara el archivo objetivo fuera del bloqueo del cargador.
La 0.1.1 y la 0.1.2 iniciaron en el equipo de prueba; la 0.1.3 requiere validación
nueva dentro del juego. `wwmi_rt_capture_boot.log` marca inicio, firma encontrada,
copia enviada y lectura después del fence; se sobrescribe en cada arranque.

En el callback anterior a ExecuteCommandLists, en su mismo hilo y cola, se envía
una lista nativa independiente con las copias y después un Signal de un fence
propio. Solo se aceptan buffers cuyo GetHeapProperties devuelve UPLOAD, que
permanecen en GENERIC_READ y no pueden ser escritos por la GPU. No se adivinan
estados ni se hacen transiciones de recursos del juego. No se lee DEFAULT,
GPU_UPLOAD ni memoria custom; se contabilizan como omitidos. El juego conserva
sus draws, dispatches y builds: los callbacks booleanos siguen devolviendo false.

La CPU solo llama Map en el READBACK propio cuando su fence completa. No se llama
Map en los recursos del juego, ni WaitForSingleObject, ni wait_idle. Se conservan
las fuentes, allocator, lista, READBACK y fence hasta completar la copia. Las
creaciones/asignaciones que pueden lanzar una excepción terminan antes del envío.
Un Signal fallido nunca cuenta como copia completada. En un unload forzado se
retienen los jobs incompletos hasta terminar el proceso, antes que liberar memoria
que la GPU podría seguir usando; no queda ningún hilo ejecutándose fuera del DLL.

Límites por captura: 128 ventanas, 16 KiB por ventana, 2 MiB de bytes copiados y
16 envíos. Los candidatos de cómputo usan hasta 32 ventanas y solo los primeros
ocho envíos pueden contener exclusivamente cómputo: se reserva capacidad para
las coincidencias raster más precisas. Cada ventana única de recurso/offset se copia una vez. Un root CBV no
informa su longitud; la ventana termina en el límite del recurso o en 16 KiB y
**puede contener asignaciones vecinas**. `extent_known=false` y
`verified_bone_palette=false` son deliberados. No se declara que una ventana
corresponda a un hueso, ni que su contenido sea la pose consumida por un draw.

`upload_readback` expone muestras, recursos/rangos/presupuesto omitidos, fallos,
ventanas sin envío y copias pendientes al guardar. Se permite hasta cinco segundos
de sondeo entre Present después de registrar; en timeout se guarda el estado
incompleto y se retienen los jobs. No se crea un archivo de ceros para una copia
sin completar. Mientras queden jobs pendientes no se inicia otra captura.

Los eventos generales y prioritarios tienen 8 MiB/32768 filas cada uno, más sus
índices/objetos; metadatos y shaders tienen 16 MiB cada uno. Hay hasta 131072
asociaciones de descriptores. Los contadores no incluyen todos los callbacks que
ya dejaron de emitir filas tras llenar su presupuesto. Las muestras de tablas
pueden ser incompletas o incluir entradas ajenas; no prueban el skinning.

## Analizar

Con Python 3.10 o posterior:

```sh
python tools/rt_capture/analyze_capture.py captura/capture.json
python tools/rt_capture/inspect_upload_windows.py captura/capture.json
python tools/rt_capture/inspect_mod.py carpeta_mod --output perfil.json
python tools/rt_capture/match_rt_geometry.py captura/capture.json perfil.json --output candidatos.json
```

El inspector de ventanas verifica tamaño, hash y procedencia de fence/UPLOAD.
Busca secuencias compatibles con filas afines 3x4 como heurística; cámaras,
objetos u otros datos también pueden pasarla. No genera un adaptador de paleta.
Los shaders anteriores a F7 siguen sin bytecode. Los datos GPU de posiciones,
instancias TLAS y registros locales SBT todavía no se leen.

El inspector del mod no edita archivos: valida buffers reales y drawindexed,
separa los rangos originales de matching de las ventanas del mod, y conserva
condiciones como texto sin ejecutar toda la lógica INI. Las variantes que
comparten la misma topología original permanecen como candidatos separados.

`SkinModPositions.hlsl` y `skin_reference.py` preparan skinning canónico de cuatro
u ocho influencias. El kernel compiló de forma independiente, pero aún no se
invoca desde el add-on. Faltan la paleta nativa verificada, shapekeys, BLAS/TLAS,
materiales/SBT, sincronización y validación visual de modelos completos en RT.

## Configuración objetivo

El formato de `wwmi_rt_target.txt` es:

```text
WWMI_RT_TARGET_V1
numero_vertices_nativos numero_componentes
primer_indice cantidad_indices
...
```

Hay un ejemplo derivado de la captura en `targets/hsin-body.target`. La firma no
contiene direcciones, hashes de shaders ni offsets de huesos adivinados. Si falta
o es inválida, la herramienta solo registra metadatos y no copia constantes.

## Compilar y validar

Desde la raíz, con Visual Studio 2022 C++ desktop y Windows SDK:

```bat
tools\rt_capture\build.cmd
```

Salida: build_addon/rt_capture/wwmi_rt_capture.addon64. Solo necesita los headers
ReShade fijados por el submódulo; no usa 3Dmigoto, ImGui ni DXC. El workflow
rt-capture.yml compila la DLL y las pruebas sin publicar releases.

```sh
g++ -std=c++20 -Wall -Wextra -Werror tests/rt_capture_test.cpp -o rt_capture_test
g++ -std=c++20 -Wall -Wextra -Werror -I tests/mock_d3d12 tests/rt_upload_readback_test.cpp -o rt_upload_readback_test
python -m unittest discover -s tests -p 'test_rt_*py'
```

Las pruebas de readback usan un doble de API determinista: verifican que no se
mapea antes del fence, que un Signal fallido no libera recursos, y que se respetan
rangos, heaps, deduplicación y presupuestos. No validan la GPU ni el driver. La DLL
entregada compila con MinGW-w64 GCC 13.2.0-posix y -Werror; el workflow MSVC aún
no se ha ejecutado. El soporte RT completo no se presenta como terminado.

Referencias primarias: [Readback con heaps](https://learn.microsoft.com/en-us/windows/win32/direct3d12/readback-data-using-heaps),
[estados de recursos UPLOAD](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12).
La conversión de unidades se contrastó con source/d3d12/d3d12_impl_type_convert.cpp
del SDK ReShade fijado en el repositorio.
