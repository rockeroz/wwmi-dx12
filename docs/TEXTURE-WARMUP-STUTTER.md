# Preparación de texturas antes del primer dibujo del mod

La captura de `e816eb0` confirma cero expiraciones del watchdog M5. El intervalo
CPU máximo entre insertar Wait y señalar el fence fue 6231 microsegundos. Los
tirones al cambiar de personaje seguían presentes y reaparecieron cargas
`TEXEAGER-NOW` desde copias o binds de cálculo. Estos datos no prueban una única
causa de todos los tirones, pero permiten corregir esa ruta síncrona concreta.

Con la opción existente `tex_background` activada (valor por defecto), el primer
dibujo identificado encola las texturas de su namespace y de sus componentes
fusionados. Mientras el trabajador termina, se deja pasar el dibujo original,
antes de publicar variables de detección, estampar texturas o ejecutar comandos
del componente. No se ejecutan copias de inicialización con texturas a medio
cargar. La siguiente evaluación que ve la publicación completa permite el mod.
La misma comprobación protege los componentes llamados desde sombras completas.

El resultado visible esperado es un breve modelo original mientras se preparan
las texturas, seguido del mod. No se precargan todos los mods de la instalación.
Los personajes ya preparados no vuelven a esperar al cambiar entre ellos, salvo
que se recargue la configuración. No requiere editar el INI. La opción
`tex_background=0` mantiene el comportamiento anterior explícito. En el overlay
es A12, «Load mod textures in the background».

El trabajador omite texturas ya suministradas por otra ruta. La publicación se
valida dentro del bloqueo del motor contra la generación que solicitó la carga;
un recurso obsoleto se libera, sin publicarlo en una configuración nueva. Un
archivo que falla también se da por terminado para evitar repetir su lectura
desde el primer dibujo. Los errores se registran y la recarga permite reintentar.
Los archivos de los mods no se modifican.

`TEXWARM` registra el inicio de la preparación y `TEXEAGER` su finalización. En
modo diferido, la duración de publicación incluye esperar a obtener el bloqueo;
se etiqueta así para no interpretarla como tiempo bloqueando el render.
Si otra ruta aún requiere `TEXEAGER-NOW`, esa línea ahora mide por separado la
carga y preparación síncrona del recurso. Esto permite comprobar si queda una
dependencia fuera del grupo preparado, sin atribuirle el tiempo del trabajador.

La prueba Windows `rt_tex_eager_test` (sustituida por `rt_tex_defer_test`, ver abajo) usaba el trabajador real con callbacks de
carga controlados en CPU: comprueba que no se espera a I/O desde el dibujo,
que el sondeo repetido no duplica tareas, que los errores finalizan, que una
recarga no pierde el estado pendiente nuevo y que se liberan recursos obsoletos.
No crea una GPU ni valida la fluidez o las texturas visibles en el juego.

Para comparar, cambiar varias veces entre los mismos personajes y conservar
`wwmi_dx12_addon.log` y `rt_observe.log`. Revisar tanto la primera aparición como
los cambios posteriores. Las compilaciones de pipelines del juego y otros
costes de primera aparición no se declaran resueltos por este cambio.

## Cambio al integrar 2026.10.4

Upstream eliminó `tex_eager` y lo sustituyó por `TexDeferWorker` (`tex_defer.*`): cada textura se carga en segundo
plano la primera vez que un dibujo la pide, no todo el namespace en el primer reclamo. Por eso las texturas de
outfits sin usar no ocupan VRAM, y el presupuesto de texturas (`tex_budget`/`tex_replace`) puede bajarlas o
descargarlas cuando no están en pantalla.

Lo que se conservó de esta rama:

- `PublishDeferredTexture(..., generation)` y `DeferredTexturePending`: el trabajador nuevo publica contra la
  generación que pidió la carga, publica `null` en un fallo (sin reintento síncrono) y no carga una textura que una
  copia ya cargó en primer plano.
- `ModTexturesReady` sigue reteniendo el dibujo del juego mientras una textura del mod se carga, ahora con
  `g_tex_defer.Pending(mod)`. Como las cargas se piden al ejecutar la sección, la primera ejecución del mod pasa
  (con las texturas del juego en los slots que aún cargan) y los dibujos siguientes esperan hasta que todo lo pedido
  se publique. Solo la primera carga de cada recarga de inis espera: después el mod queda "caliente" y una textura
  que se carga más tarde (descargada por el presupuesto y pedida otra vez, o usada por primera vez por una parte o
  toggle) deja el mod dibujado con la textura del juego en ese slot hasta que llega. Antes, cada recarga devolvía
  todo el personaje al modelo original, visto como partes del original parpadeando con RT.
- `TEXEAGER-NOW` se llama ahora `TEXDEFER-NOW` y mantiene la medición del tiempo de carga síncrona.

`RtForgetModTextures` (llamado desde `tex_replace` al bajar, descargar o liberar una textura de mod) borra los
stamps de RT que apuntan a ella, para que `M4Apply` no copie a una tabla de materiales RT el descriptor de una
textura liberada. Además `SlotTexFor`/`RasterSwapFor` marcan como usada una textura que los rayos leen, para que
el presupuesto no la cuente como fuera de vista mientras el personaje aparece solo en reflejos.

La prueba Windows `rt_tex_defer_test` usa el `TexDeferWorker` real con callbacks de carga controlados en CPU:
comprueba que la carga no corre en el hilo de dibujo, que el mod queda pendiente desde que se pide, que una
textura ya cargada no se vuelve a cargar, que los fallos terminan, que una carga de una generación anterior no se
publica y que no quedan recursos vivos.
