# M5: descriptores exclusivos y publicación sincronizada

Base: `cb8f5f9`, rama de trabajo `rt-m5-critical-lifetimes`.

Esta rama corrige los dos puntos críticos de la revisión: usar espacio supuesto
libre del heap del juego y rellenar datos provisionales sin proteger su copia GPU.
Mantiene las funciones M3/M4/M5 existentes; requiere prueba en el juego.

## Descriptores

- Al crear un heap CBV/SRV/UAV shader-visible grande, se solicitan 65536
  descriptores adicionales. `GetDesc` sigue devolviendo el tamaño solicitado por
  el juego; la cola añadida pertenece exclusivamente al módulo.
- No se toma espacio de heaps anteriores ni se infiere disponibilidad por el
  mayor slot observado. Si la reserva no existe o se agota, la tabla se rechaza.
- Las tablas tienen leases compartidos por sus bloques, copias y listas lectoras.
  La región deja de rotar por `frame % 3`.
- Un hook del ExecuteCommandLists nativo señala el fence de cada cola **después**
  de enviar las listas. Los recursos siguen retenidos por las grabaciones y por
  todos sus envíos en vuelo; resetear una lista en CPU no libera trabajo GPU.

## Datos provisionales

- Cada copia extendida prepara una instantánea propia de la tabla base y del
  bloque M5. No vuelve a leer en GPU la memoria UPLOAD del juego.
- El bloque se asocia al frame, dirección fuente y secuencia de la copia que
  produjo la tabla. No se reutilizan bloques de una dirección de otro frame.
- Una copia provisional tiene su propio fence de publicación. Antes de enviar
  cualquier lista que la lea se inserta una espera en esa cola, aunque sea otra
  cola distinta de la compuerta del TLAS.
- Solo una transacción M3 confirmada puede rellenar y publicar el payload.
  Una publicación terminada es inmutable; los rellenos tardíos se rechazan.
- Al terminar el frame, antes de Present, los payloads que no recibieron un
  relleno coincidente congelan el fallback y abren su compuerta inmediatamente.
  Un trabajador independiente conserva el plazo de 50 ms como salvaguarda si
  no llega ese cierre de frame. No se reduce la ventana mientras aún se graban
  los dispatches directos del frame. El fallback contiene identificadores de hit shader nulos
  en toda la tabla: evita combinar registros viejos con geometría nueva.
  Puede faltar la contribución RT de esa pasada. No se presenta como una solución
  visual para todas las transiciones: el objetivo es un fallo controlado y medible.

## Instalación y prueba

1. Instalar el paquete de `RT observe build` de esta rama con el juego cerrado.
   La reserva se realiza al crear los heaps; no se admite actualizar el módulo en
   caliente conservando grabaciones antiguas.
2. Conservar el INI existente. No se añadieron opciones obligatorias.
3. Probar primero los mismos Hsin/Qingxiao que funcionaban en `6a096f9`, cambiar
   de personaje y de material, y revisar reflejos y sombras.
4. Guardar `wwmi_dx12_addon.log`, `ReShade.log` y `rt_observe.log` con el commit
   exacto del paquete. Si se produce un cierre, conservar el diagnóstico DRED.

`RTM5SAFE` informa de heaps reservados, asignaciones, submissions/completadas,
publicaciones, fallbacks y rellenos tardíos rechazados. La línea de inicio confirma
si se instaló la reserva. Un fallo de instalación no autoriza usar la cola original
del heap: M5 conserva el rechazo explícito.

## Validación disponible

- `rt_m5_policy_test`: límites, agotamiento, coalescencia y publicación única.
- `rt_m5_native_test`: dobles COM en CPU, sin crear una GPU. Comprueba el ABI x64
  de GetDesc, la capacidad añadida, el fence posterior a ExecuteCommandLists,
  dos colas lectoras, el caso de remoción, la espera de publicación y el rechazo
  de rellenos posteriores al timeout.
- Las pruebas existentes del registro y del análisis offline siguen aplicándose.
- La compilación y estas pruebas no sustituyen la prueba de rendimiento y
  funcionamiento en el juego. La instantánea CPU de la tabla añade trabajo que
  debe medirse; el resto de riesgos M3/M4 de la revisión no se declara resuelto.

## Regresión al cambiar de personaje (prueba 08b2546)

El registro recibido acumuló 1499 payloads provisionales: 1301 publicados y 198
fallbacks. La versión anterior dejaba los que no tenían relleno hasta el plazo
del trabajador, incluso cuando el frame ya había terminado. Ahora se cierran
al final del frame, con las mismas tablas nulas inmutables y leases GPU.

`RTM5WAIT` distingue cierres de frame y expiraciones del trabajador, y cuenta
las esperas insertadas. `wait_signal_us` y `max_wait_signal_us` miden tiempo CPU
desde la primera inserción de Wait hasta Signal de cada payload: no son tiempo
de ejecución GPU ni demuestran por sí solos cuánto se detuvo la GPU.

Las pruebas nativas comprueban el cierre inmediato del fence, la inmutabilidad,
el cierre antes de enviar una lista y que no reemplaza datos reales publicados.
La captura también muestra cargas síncronas de texturas en TEXEAGER-NOW. Este
cambio no modifica esa ruta: aún puede producir tirones de primera carga. El
tiempo reportado por TEXEAGER como «under the engine lock» incluye esperar a
adquirir ese bloqueo; no debe interpretarse entero como bloqueo del render.
