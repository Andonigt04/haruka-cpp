#include "RHI/Common/RHITestCommon.h"

// ────────────────────────────────────────────────────────────────────────────────────────────────
// ATRIBUTO DE VÉRTICE QUE NO ES LA POSICIÓN
//
// Los props sacan su color del COLOR POR VÉRTICE (`baseColor = Color` en prop_inst.frag), y en
// Vulkan salían casi BLANCOS con las texturas correctas y la máscara de material bien puesta. Si un
// atributo distinto de la posición no llega, el color por vértice sería (0,0,0) o basura y el
// resultado, blanco o negro. Esto lo aísla: un triángulo con color por vértice y lectura del píxel.
// ================================================================================================
// ¿INTERPOLA el RHI los atributos, o cada cara sale de un color?
//
// ⚠️ ESTE HUECO LLEVABA AHI DESDE QUE SE CREO EL RHI, y lo dice el propio `testVertexColor`: *"los
// tres vertices del MISMO color: asi el centro es ese color exacto y no hay que razonar sobre la
// interpolacion"*. Comprueba que el atributo LLEGA, pero con los tres iguales **un fallo de
// interpolacion pasa el test sin despeinarse**: si el triangulo entero saliera plano, el pixel
// central seria exactamente el mismo color y todo verde.
//
// Andoni: *"toda la cara/triangulo se dibuja con la misma iluminacion o color"* y *"es un error que
// ya existia desde la creacion del RHI"*. Esto es lo que faltaba para poder decirlo con un numero:
// UN triangulo con TRES colores distintos, y se leen cuatro puntos. Si interpola, cada esquina tira
// hacia su vertice y el centro es la mezcla; si no, los cuatro salen iguales.
/**
 * @brief Los shaders del ICONO DE INVENTARIO pintan, y pintan IGUAL en los dos backends.
 *
 * El preview de items vivia en OpenGL crudo (glCreateShader + FBO propio), asi que bajo Vulkan las
 * casillas salian sin icono. Al portarlo al RHI aparecen dos riesgos que NO se ven en pantalla hasta
 * que alguien abre el inventario:
 *
 *  1. Que el pipeline no pinte NADA. El icono usa una ortografica normal con test Less, mientras que
 *     todo el motor va en reversed-Z (limpia la profundidad a 0 y compara con Greater). Heredar ese
 *     0.0 deja la textura VACIA sin un solo error — y una textura vacia y una casilla sin objeto se
 *     ven exactamente igual. Ese fallo mudo ya ocurrio una vez en la version de GL.
 *  2. Que salga DEL REVES solo en Vulkan. La Y del clip de Vulkan va invertida y este motor la
 *     compensa en la PROYECCION (Camera.cpp), no en el viewport; el icono tiene proyeccion propia y
 *     por tanto necesita su propia compensacion.
 *
 * Por eso se dibuja una caja ALTA y DESCENTRADA (no simetrica): un objeto simetrico saldria igual del
 * derecho que del reves y el test no distinguiria nada. Se mide cuanta tinta cae en la mitad de
 * arriba frente a la de abajo. Si los dos backends dan el mismo reparto, ImGui mostrara lo mismo en
 * los dos: muestrea el texel (0,0) en ambos, asi que arrays de texeles iguales = imagen igual.
 */
// ── EL MAR SOBRE CUALQUIER FORMA, Y LA OLA QUE SE VE == LA OLA QUE SE NADA ──────────────────────
//
// La ola vive DOS VECES: `assets/shaders/lib/ocean_wave.glsl` (la que se dibuja) y
// `src/Haruka/Core/planet/ocean_wave.h` (la que usa la fisica para flotar y arrastrar). Estan escritas como
// gemelas, cada una con comentarios que apuntan a la otra, y NO HABIA NI UN TEST QUE LAS COMPARASE.
// Si divergen, lo que se ve y lo que se nada dejan de ser lo mismo — y esa es exactamente la clase de
// fallo que no da error, no rompe nada y solo se nota como "el agua esta rara".
//
// Se sube a la GPU el MISMO banco de trenes que usa la CPU (`oceanDefaultState`), asi que cualquier
// diferencia es de la formula y no de los datos.

/// Gemelo del bloque `OceanParams` de lib/ocean_params.glsl (binding 29).
// ⚠️ GEMELO A MANO DEL BLOQUE `OceanParams`, Y EL TAMAÑO VA POR `OCEAN_WAVES`, NO POR UN LITERAL.
// Estaba como `float wave[4][4]`. Al subir el espectro a 8 trenes, rellenarlo escribia 16 floats MAS
// ALLA del struct: **volcado de nucleo**, el banco entero muerto. Y el sintoma previo, con el bucle
// aun en 4, fue el contrario y mas silencioso — el agua no dibujaba un pixel porque los trenes
// vacios daban `k = 2π/0`. Un gemelo a mano con una constante escrita rompe en las dos direcciones.
struct OceanParamsUBO { float wave[Haruka::World::Water::OCEAN_WAVES][4]; float misc[4];
                        float phase[2][4]; float anchor[4]; };
static_assert(sizeof(OceanParamsUBO) == (Haruka::World::Water::OCEAN_WAVES + 4) * 16,
              "OceanParams del banco descuadrado del bloque GLSL (8 trenes + misc + 2 fases + ancla)");

/// `anchorRelEye`: el ancla del mar relativa al ojo (0 = el marco plano de las sondas). `clockS`:
/// el reloj del oleaje (0 = lo pone cada sonda).
static BufferHandle makeOceanStateUBO(const Haruka::World::Water::OceanState& st,
                                      const glm::vec3& anchorRelEye = glm::vec3(0.0f), float clockS = 0.0f)
{
    OceanParamsUBO u{};
    // ⚠️ AQUI HABIA UN `4` ESCRITO A MANO Y COSTO EL PASE DE AGUA ENTERO. Al subir el espectro de 4 a
    // 8 trenes, los cuatro ultimos se quedaban a CERO en el UBO: `lambda = 0` da `k = 2π/0 = inf`, el
    // vertice sale NaN y **no se dibuja un solo pixel de agua**. El sintoma no fue "el mar se ve
    // raro", fue "el mar no existe", y en UN backend de los dos. El bucle va con `OCEAN_WAVES`.
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i)
        for (int c = 0; c < 4; ++c) u.wave[i][c] = st.wave[i][c];
    u.misc[0] = st.seaLevelM;
    u.misc[1] = 1.0f;              // "el bloque trae datos validos": sin esto el shader usaria su
                                   // tabla de respaldo y el careo no probaria nada del estado subido.
    // ⚠️ Y LOS OTROS DOS CAMPOS, QUE SE QUEDABAN EN CERO. `misc.w` es la ESCALA DE ESPUMA
    // (`harukaFoamScale`), asi que un cero ahi apaga la espuma de la GPU ENTERA: el careo de espuma
    // salia GPU 0,0000 contra CPU 1,0000 a 0,50 m de fondo — el unico fallo que quedaba en Vulkan, y
    // no era del motor sino de este ayudante, que no copiaba el `misc` que sube `setOceanState`.
    // `misc.z` es el reloj del oleaje; aqui va a 0 A PROPOSITO (cada sonda pasa su propio tiempo y
    // el banco tiene que ser determinista), pero se deja escrito para que se vea que es una eleccion.
    u.misc[2] = clockS;                                  // reloj: lo pone cada sonda
    u.misc[3] = Haruka::World::Water::oceanFoamScale();        // gemelo de `TerrestrialPlanet::setOceanState`
    // Desfases del estado (0 salvo que la sonda los ponga) y el ANCLA (por defecto en el origen: las
    // sondas del banco ya pasan posiciones "relativas" en marco plano, asi que el careo mide la
    // misma formula que antes). El viento conocido, como lo sube `setOceanState`.
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) u.phase[i / 4][i % 4] = st.phase[i];
    u.anchor[0] = anchorRelEye.x; u.anchor[1] = anchorRelEye.y; u.anchor[2] = anchorRelEye.z;
    u.anchor[3] = st.windMS;
    return g_dev->createBuffer(BufferUsage::Uniform, sizeof(u), &u, BufferMemory::Dynamic);
}

/**
 * @brief EL PLIEGUE, EN LA GPU: la componente HORIZONTAL careada, y la malla medida con SU aritmetica.
 *
 * ⚠️ EL CAREO DE LA OLA MIRABA LA ALTURA Y LA ESPUMA, Y EL VUELCO NO VIVE EN NINGUNA DE LAS DOS. Lo
 * que vuelca una cresta es el vaiven HORIZONTAL (`harukaHorizAmp` por el refuerzo direccional de
 * rompiente), y ese termino no pasaba por ninguna comparacion CPU<->GPU: las dos implementaciones
 * podian divergir justo en lo que decide si una ola rompe, y el banco daba verde. Es el mismo hueco
 * que tenia la espuma antes de `oceanFoam`.
 *
 * Y hay una segunda mitad que solo se puede hacer aqui: `ocean_mesh_stretch` mide el avance entre
 * vertices vecinos con la aritmetica de la CPU. **Lo que dibuja es la GPU.** Si el fp64 degradado, un
 * `fma` distinto o un builtin con otro redondeo movieran el termino horizontal, la malla se
 * invertiria en sitios distintos de los que la CPU predice — y la CPU seguiria diciendo que todo
 * bien. Aqui el conteo de celdas del reves se hace sobre los valores que ha devuelto la GPU.
 */
void testOceanFoldParity()
{
    BEGIN("mar: el PLIEGUE de la GPU == el de la CPU (la horizontal, no la altura)");

    const std::string base = Haruka::Renderer::Shader::baseDir();
    const std::string vs = base + "shaders/tests/rhitest_fullscreen.vert";
    const std::string fs = base + "shaders/tests/rhitest_waveprobe.frag";

    // Mismo tamano y mismo motivo que `testOceanWaveParity`: el viewport de GL es estado GLOBAL.
    const int W = 256, H = 256;
    // ⚠️ CERCA DEL ORIGEN. Esto acaba en una diferencia finita entre pixeles vecinos, y a radio
    // planetario 1 ulp de float32 son ~0,5 m: la resta saldria cuantizada y el pliegue medido seria
    // el del redondeo. La misma piedra que tumbo la primera version de `ocean_mesh_stretch`.
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 origin(1000.0f, 0.0f, -500.0f);
    // ⚠️ EL PASO ES LA CELDA DE LA MALLA DEL AGUA, no un numero cualquiera: lo que se mide es si dos
    // vertices VECINOS se cruzan, asi que la separacion tiene que ser la que el pase de agua usa.
    const float     q = 0.6f;                       // texel x zancada del nodo, a los pies
    // ⚠️ EL PASO VA A LO LARGO DE `t1`, EL MISMO EJE SOBRE EL QUE SE PROYECTA EL DESPLAZAMIENTO. Mi
    // primera version avanzaba en X del mundo mientras medía la componente sobre `t1`, y con este
    // `up` esos dos ejes son PERPENDICULARES: estaba midiendo un termino CRUZADO
    // (`d(disp·t1)/dt2`), que no es el que pliega. Salian 0 celdas invertidas de 261 120 mientras la
    // CPU encontraba 87 de 48 000 — y el numero parecia una buena noticia.
    const float     t0 = 12.75f;
    const float     depth0 = 1.0f, depthStep = 0.05f;   // 1,0 .. 13,75 m: la barra donde vuelca

    // El marco tangente, EL MISMO que construyen `harukaGerstner` y `oceanDisplacement`.
    const glm::vec3 t1 = glm::normalize(std::abs(up.y) < 0.99f ? glm::cross(up, glm::vec3(0,1,0))
                                                               : glm::cross(up, glm::vec3(1,0,0)));
    const glm::vec3 t2 = glm::cross(up, t1);
    const glm::vec3 stepX = t1 * q;                 // columnas: a lo largo de t1
    const glm::vec3 stepY = t2 * (q * 0.37f);       // filas: otra franja de mar, no la misma linea
    // ⚠️ CON PENDIENTE DEL FONDO. Sin ella no hay refraccion, y sin refraccion la cresta NO vuelca —
    // el barrido daba 0 celdas plegadas y el careo comparaba 0 contra 0. Es la misma direccion que
    // usa `ocean_mesh_stretch` en CPU: hacia donde el fondo SUBE, o sea hacia la orilla.
    const glm::vec3 slope = t1;

    struct ProbeUBO { float origin[4], stepX[4], stepY[4], up[4], depth[4]; } pu{};
    pu.origin[0]=origin.x; pu.origin[1]=origin.y; pu.origin[2]=origin.z; pu.origin[3]=t0;
    pu.stepX[0]=stepX.x;   pu.stepX[1]=stepX.y;   pu.stepX[2]=stepX.z;   pu.stepX[3]=slope.x;
    pu.stepY[0]=stepY.x;   pu.stepY[1]=stepY.y;   pu.stepY[2]=stepY.z;   pu.stepY[3]=slope.y;
    pu.up[0]=up.x;         pu.up[1]=up.y;         pu.up[2]=up.z;         pu.up[3]=slope.z;
    pu.depth[0]=depth0;    pu.depth[1]=depthStep; pu.depth[2]=(float)H;

    PipelineDesc pd;
    pd.vertexPath = vs.c_str(); pd.fragmentPath = fs.c_str();
    pd.depth.test = false; pd.depth.write = false; pd.blend.enable = false;
    PipelineHandle pipe = g_dev->createPipeline(pd);
    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
    BufferHandle ocean = makeOceanStateUBO(st);
    BufferHandle ub = g_dev->createBuffer(BufferUsage::Uniform, sizeof(pu), &pu, BufferMemory::Dynamic);
    CHECK(valid(pipe) && valid(ub) && valid(ocean), "pipeline y UBOs de la sonda creados");
    if (!valid(pipe)) return;

    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    std::vector<uint8_t> px((size_t)W * H * 4, 0xAA);
    auto disparar = [&](int modo) {
        pu.depth[3] = (float)modo;
        g_dev->updateBuffer(ub, 0, sizeof(pu), &pu);
        if (Context* c = g_dev->beginFrame()) {
            ClearValues cv;
            cv.clearColor = true; cv.color[0]=cv.color[1]=cv.color[2]=0.0f; cv.color[3]=1.0f;
            cv.clearDepth = true; cv.depth = 0.0f;
            c->beginRenderPass({}, cv);
            c->setViewport(0, 0, W, H);
            c->bindPipeline(pipe);
            c->bindUniformBuffer(0, ub);
            c->bindUniformBuffer(29, ocean);
            c->draw(3);
            c->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk)  g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        }
    };
    // 24 bits sobre ±16 m: 1,9 µm de cuantizacion. El mismo desempaquetado del shader, al reves.
    auto decodeX = [&](int x, int y) {
        const size_t k = ((size_t)y * W + x) * 4;
        const float t = (float)(px[k] * 65536u + px[k+1] * 256u + px[k+2]) / 16777215.0f;
        return (t * 2.0f - 1.0f) * 16.0f;
    };

    // ── 1) LA HORIZONTAL, CAREADA ───────────────────────────────────────────────────────────────
    double peor = 0.0, suma = 0.0; int n = 0, peorY = 0;
    float peorGpu = 0.0f, peorCpu = 0.0f;
    std::vector<float> gpuX((size_t)W * H, 0.0f);      // guardada para la fase 2
    for (int modo = 1; modo <= 2; ++modo) {
        disparar(modo);
        const glm::vec3 eje = (modo == 1) ? t1 : t2;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const glm::vec3 wp = origin + stepX * (float)x + stepY * (float)y;
                const float depth  = depth0 + depthStep * (float)y;
                const glm::vec3 d  = Haruka::World::Water::oceanDisplacement(wp, up, t0, depth, 1.0f, st,
                                                                   Haruka::World::Water::WATER_FETCH_UNLIMITED,
                                                                   slope);
                const float cpu = glm::dot(d, eje);
                const float gpu = decodeX(x, y);
                if (modo == 1) gpuX[(size_t)y * W + x] = gpu;
                const double e = std::abs((double)gpu - (double)cpu);
                suma += e; ++n;
                if (e > peor) { peor = e; peorY = y; peorGpu = gpu; peorCpu = cpu; }
            }
    }
    std::printf("    %d muestras (dos tangentes) · profundidad de %.2f a %.2f m\n",
                n, depth0, depth0 + depthStep * (H - 1));
    std::printf("    vaiven HORIZONTAL, GPU vs CPU: media %.6f m · peor %.6f m\n",
                n ? suma / n : 0.0, peor);
    std::printf("      peor a %.2f m de fondo: GPU %+.4f m · CPU %+.4f m\n",
                depth0 + depthStep * peorY, peorGpu, peorCpu);
    // La cuantizacion del canal son 1,9 µm; el resto es `sin`/`tanh` de dos implementaciones. Un
    // milimetro es holgado para eso y estrecho para una divergencia de formula (el termino del
    // vuelco vale METROS).
    CHECK(peor < 1e-3, "el vaiven horizontal dibujado y el simulado son el mismo (< 1 mm)");

    // ── 2) EL PLIEGUE, CONTADO CON LOS VALORES DE LA GPU ────────────────────────────────────────
    //
    // `avance = q + (dispX[x+1] − dispX[x])`, normalizado por `q`. <= 0 = los dos vertices se han
    // cruzado: la celda esta del reves. Es la misma cuenta de `ocean_mesh_stretch`, con la diferencia
    // que da sentido a este test — los numeros los ha producido la GPU.
    //
    // ⚠️ VARIOS INSTANTES, NO UNO. El pliegue es el 0,18 % de las celdas y depende de la FASE: con un
    // solo `t` el barrido daba 0 celdas plegadas en los dos lados y la guarda "atraviesa la zona que
    // pliega" fallaba — no porque el motor no plegara, sino porque la foto caia entre dos crestas.
    int invGpu = 0, invCpu = 0, tot = 0;
    double minGpu = 1e9, maxGpu = -1e9;
    const float tiempos[] = { t0, 31.4f, 57.9f, 88.2f };
    for (float tt : tiempos) {
    pu.origin[3] = tt;
    disparar(1);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x + 1 < W; ++x) {
            gpuX[(size_t)y * W + x] = decodeX(x, y);
            gpuX[(size_t)y * W + x + 1] = decodeX(x + 1, y);
            const float depth = depth0 + depthStep * (float)y;
            const double avG = 1.0 + ((double)gpuX[(size_t)y * W + x + 1]
                                    - (double)gpuX[(size_t)y * W + x]) / (double)q;
            const glm::vec3 a = origin + stepX * (float)x       + stepY * (float)y;
            const glm::vec3 b = origin + stepX * (float)(x + 1) + stepY * (float)y;
            const double avC = 1.0 + ((double)glm::dot(Haruka::World::Water::oceanDisplacement(b, up, tt, depth, 1.0f, st, Haruka::World::Water::WATER_FETCH_UNLIMITED, slope), t1)
                                    - (double)glm::dot(Haruka::World::Water::oceanDisplacement(a, up, tt, depth, 1.0f, st, Haruka::World::Water::WATER_FETCH_UNLIMITED, slope), t1)) / (double)q;
            if (avG <= 0.0) ++invGpu;
            if (avC <= 0.0) ++invCpu;
            minGpu = std::min(minGpu, avG); maxGpu = std::max(maxGpu, avG);
            ++tot;
        }
    }
    pu.origin[3] = t0;
    std::printf("    celdas del reves segun la GPU: %d de %d · segun la CPU: %d · "
                "avance %.3f .. %.3f\n", invGpu, tot, invCpu, minGpu, maxGpu);
    // ⚠️ NO SE EXIGE CERO: el vuelco es deliberado (ver `ocean_mesh_stretch`). Lo que se exige es que
    // las DOS implementaciones plieguen LO MISMO. Si la GPU plegara donde la CPU no, la fisica y el
    // render dejarian de describir la misma ola justo en la rompiente.
    CHECK(invCpu > 0, "el barrido ATRAVIESA la zona que pliega (si no, se compararia 0 contra 0)");
    CHECK(std::abs(invGpu - invCpu) <= tot / 1000,
          "la GPU y la CPU pliegan las MISMAS celdas (±0,1 %)");

    // CONTRAPRUEBA: con el tren dominante un 50 % mas alto la horizontal cambia de verdad. Sin esto,
    // "coinciden" podria significar que el careo no mide nada.
    {
        Haruka::World::Water::OceanState alt = st;
        alt.wave[0][1] *= 1.5f;
        double peorAlt = 0.0;
        for (int y = 0; y < H; y += 4)
            for (int x = 0; x < W; x += 4) {
                const glm::vec3 wp = origin + stepX * (float)x + stepY * (float)y;
                const float depth  = depth0 + depthStep * (float)y;
                const float cpuAlt = glm::dot(Haruka::World::Water::oceanDisplacement(wp, up, t0, depth, 1.0f, alt,
                                                                               Haruka::World::Water::WATER_FETCH_UNLIMITED,
                                                                               slope), t1);
                peorAlt = std::max(peorAlt, std::abs((double)gpuX[(size_t)y * W + x] - (double)cpuAlt));
            }
        std::printf("    CONTRAPRUEBA: con el tren dominante un 50%% mas alto, la peor diferencia "
                    "sube a %.4f m\n", peorAlt);
        CHECK(peorAlt > 0.05, "el careo de la horizontal DETECTA un cambio del estado de mar");
    }

    g_dev->destroy(pipe); g_dev->destroy(ub); g_dev->destroy(ocean);
}

void testOceanWaveParity()
{
    BEGIN("mar: la ola de la GPU == la ola de la CPU (gemelas)");

    const std::string base = Haruka::Renderer::Shader::baseDir();
    const std::string vs = base + "shaders/tests/rhitest_fullscreen.vert";
    const std::string fs = base + "shaders/tests/rhitest_waveprobe.frag";

    // Barrido determinista: cada pixel es una muestra. La profundidad recorre de 0,5 m a ~12 m, o
    // sea toda la franja de rompiente, que es donde el modelo hace mas cosas y donde estaba el bug
    // del tope por tren. Con solo mar abierto el careo pasaria sin tocar nada de eso.
    // ⚠️ EL TAMANO ES EL DE LA VENTANA DEL BANCO (256x256) A PROPOSITO. En OpenGL el viewport es
    // estado GLOBAL: un test que lo deja en 128x128 hace que TODOS los siguientes dibujen en una
    // esquina mientras leen la ventana entera. Costo 6 fallos en tests que no tienen nada que ver
    // (cielo, descarte de caras, costuras, luz de props) y solo en GL — en Vulkan el viewport es
    // estado dinamico del pase y se refija solo, asi que la tanda de Vulkan salia limpia y la de GL
    // no. Si algun dia hace falta otro tamano, hay que restaurarlo antes de endRenderPass.
    const int W = 256, H = 256;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    const glm::vec3 origin(1000.0f, 0.0f, -500.0f);
    const glm::vec3 stepX(0.7f, 0.0f, 0.11f);
    const glm::vec3 stepY(0.13f, 0.0f, 0.9f);
    const float     t0 = 12.75f;
    const float     depth0 = 0.5f, depthStep = 0.09f;

    struct ProbeUBO {
        float origin[4], stepX[4], stepY[4], up[4], depth[4];
    } pu{};
    pu.origin[0]=origin.x; pu.origin[1]=origin.y; pu.origin[2]=origin.z; pu.origin[3]=t0;
    pu.stepX[0]=stepX.x;   pu.stepX[1]=stepX.y;   pu.stepX[2]=stepX.z;
    pu.stepY[0]=stepY.x;   pu.stepY[1]=stepY.y;   pu.stepY[2]=stepY.z;
    pu.up[0]=up.x;         pu.up[1]=up.y;         pu.up[2]=up.z;
    pu.depth[0]=depth0;    pu.depth[1]=depthStep; pu.depth[2]=(float)H;

    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.depth.test = false; pd.depth.write = false;
    pd.blend.enable = false;
    pd.cull = CullMode::None;

    PipelineHandle pipe = g_dev->createPipeline(pd);
    BufferHandle   ub   = g_dev->createBuffer(BufferUsage::Uniform, sizeof(pu), &pu, BufferMemory::Dynamic);
    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
    BufferHandle   ocean = makeOceanStateUBO(st);
    CHECK(valid(pipe) && valid(ub) && valid(ocean), "pipeline y UBOs de la sonda creados");
    if (!valid(pipe)) return;

    std::vector<uint8_t> px((size_t)W * H * 4, 0xAA);
    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    if (Context* c = g_dev->beginFrame()) {
        ClearValues cv;
        cv.clearColor = true; cv.color[0]=cv.color[1]=cv.color[2]=0.0f; cv.color[3]=1.0f;
        cv.clearDepth = true; cv.depth = 0.0f;
        c->beginRenderPass({}, cv);
        c->setViewport(0, 0, W, H);
        c->bindPipeline(pipe);
        c->bindUniformBuffer(0, ub);
        c->bindUniformBuffer(29, ocean);
        c->draw(3);
        c->endRenderPass();
        if (!isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        g_dev->endFrame();
        pumpWindowEvents();
        if (isVk)  g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
    }

    // ⚠️ Si el readback no escribe, el buffer sigue en 0xAA y decodificarlo daria una altura
    // constante que casaria mal con TODO — y el test acusaria al shader de un fallo del instrumento.
    bool wrote = false;
    for (size_t k = 0; k < px.size(); k += 4) if (px[k] != 0xAA || px[k+1] != 0xAA) { wrote = true; break; }
    CHECK(wrote, "el readback escribio (si no, el careo no significaria nada)");
    if (!wrote) { std::printf("    SIN LECTURA: no se puede carear\n"); return; }

    const float kRange = 4.0f;   // el mismo que codifica el shader
    auto decode = [&](int x, int y) {
        const size_t k = ((size_t)y * W + x) * 4;
        const float scaled = px[k] * 65536.0f + px[k+1] * 256.0f + px[k+2];
        return (scaled / 16777215.0f) * 2.0f * kRange - kRange;
    };

    // La espuma viaja en el ALFA con 8 bits (ver `rhitest_waveprobe.frag`).
    auto decodeFoam = [&](int x, int y) {
        return px[((size_t)y * W + x) * 4 + 3] / 255.0f;
    };

    double worst = 0.0, sum = 0.0; int n = 0, worstX = 0, worstY = 0;
    float worstGpu = 0.0f, worstCpu = 0.0f;
    double foamWorst = 0.0; int foamWorstY = 0; float foamGpu = 0.0f, foamCpu = 0.0f;
    int foamWet = 0;                              // muestras con espuma apreciable, para que no pase
                                                  // por verde un careo de 0 contra 0
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const glm::vec3 wp = origin + stepX * (float)x + stepY * (float)y;
            const float depth  = depth0 + depthStep * (float)y;
            const float hCpu = Haruka::World::Water::oceanWaveHeight(wp, up, t0, depth, 1.0f, st);
            const float hGpu = decode(x, y);
            const double d = std::abs((double)hGpu - (double)hCpu);
            sum += d; ++n;
            if (d > worst) { worst = d; worstX = x; worstY = y; worstGpu = hGpu; worstCpu = hCpu; }

            // ⚠️ LA ESPUMA, que hasta ahora no se careaba con nada. Mismo punto, mismo tiempo.
            const float fCpu = Haruka::World::Water::oceanFoam(wp, up, t0, depth, 1.0f, st);
            const float fGpu = decodeFoam(x, y);
            if (fCpu > 0.01f) ++foamWet;
            const double df = std::abs((double)fGpu - (double)fCpu);
            if (df > foamWorst) { foamWorst = df; foamWorstY = y; foamGpu = fGpu; foamCpu = fCpu; }
        }

    const double mean = n ? sum / n : 0.0;
    std::printf("    %d muestras · profundidad de %.2f a %.2f m (la franja de rompiente entera)\n",
                n, depth0, depth0 + depthStep * (H - 1));
    std::printf("    diferencia GPU vs CPU: media %.6f m · peor %.6f m\n", mean, worst);
    std::printf("      peor punto (%d,%d) a %.2f m de fondo: GPU %+.4f m · CPU %+.4f m\n",
                worstX, worstY, depth0 + depthStep * worstY, worstGpu, worstCpu);

    // El suelo de ruido no es cero: la altura viaja codificada en 24 bits sobre un rango de +-4 m,
    // o sea ~0,5 micras de cuantizacion, y las dos partes usan float de 32 bits con `sin` de
    // implementaciones distintas. Un milimetro es holgado para eso y estrecho para una divergencia
    // real de formula (el bug del tope por tren daba METROS).
    CHECK(worst < 1e-3, "la ola dibujada y la ola simulada son la misma (< 1 mm)");

    // ── LA ESPUMA ────────────────────────────────────────────────────────────────────────────────
    // Era la unica magnitud de la ola sin gemelo en CPU, y por tanto sin careo posible. Ahora
    // `oceanFoam` existe y se compara en el mismo barrido.
    std::printf("    espuma: %d de %d muestras con espuma apreciable (>0,01)\n", foamWet, n);
    std::printf("      diferencia GPU vs CPU: peor %.6f (fila %d, %.2f m de fondo): GPU %.4f · CPU %.4f\n",
                foamWorst, foamWorstY, depth0 + depthStep * foamWorstY, foamGpu, foamCpu);
    // ⚠️ SIN ESTA GUARDIA EL CAREO DE ESPUMA SERIA UNA TAUTOLOGIA: si el barrido cayera entero en agua
    // sin espuma, comparar 0 contra 0 daria "paridad perfecta" sin haber probado una sola rama.
    CHECK(foamWet > n / 100, "el barrido ATRAVIESA espuma (si no, el careo no probaria nada)");
    // La tolerancia es la cuantizacion del canal: 1/255 = 0,0039. Se deja el doble por el redondeo de
    // los dos lados. Una divergencia de FORMULA (un umbral movido en un solo lado) daria decimas.
    CHECK(foamWorst < 2.0 / 255.0, "la espuma dibujada y la espuma simulada son la misma (< 2/255)");

    // CONTRAPRUEBA: con el estado CAMBIADO la diferencia tiene que dispararse. Sin esto, un careo que
    // comparase cualquier cosa consigo misma daria 0 y pasaria igual.
    Haruka::World::Water::OceanState alt = st;
    alt.wave[0][1] *= 1.5f;                       // el tren dominante, un 50% mas alto
    double worstAlt = 0.0, foamAlt = 0.0;
    for (int y = 0; y < H; y += 4)
        for (int x = 0; x < W; x += 4) {
            const glm::vec3 wp = origin + stepX * (float)x + stepY * (float)y;
            const float depth  = depth0 + depthStep * (float)y;
            const float hAlt = Haruka::World::Water::oceanWaveHeight(wp, up, t0, depth, 1.0f, alt);
            worstAlt = std::max(worstAlt, std::abs((double)decode(x, y) - (double)hAlt));
            const float fAlt = Haruka::World::Water::oceanFoam(wp, up, t0, depth, 1.0f, alt);
            foamAlt = std::max(foamAlt, std::abs((double)decodeFoam(x, y) - (double)fAlt));
        }
    std::printf("    CONTRAPRUEBA: con el tren dominante un 50%% mas alto, la peor diferencia sube a %.4f m\n",
                worstAlt);
    std::printf("      y la de espuma a %.4f\n", foamAlt);
    CHECK(worstAlt > 0.05, "el careo detecta un cambio del estado (no compara algo consigo mismo)");
    // La misma contraprueba para la espuma: si `oceanFoam` ignorase el estado (devolviendo, por
    // ejemplo, solo la rama de orilla por profundidad) esto seguiria en cero y el careo de arriba
    // pasaria sin probar que la espuma depende de la OLA.
    CHECK(foamAlt > 0.05, "el careo de espuma detecta un cambio del estado");

    g_dev->destroy(pipe); g_dev->destroy(ub); g_dev->destroy(ocean);
}

void testWaterShapes()
{
    BEGIN("mar: la ola sobre un anillo y sobre un cubo (no hace falta planeta)");

    const std::string base = Haruka::Renderer::Shader::baseDir();
    const std::string vs = base + "shaders/tests/rhitest_watershape.vert";
    const std::string fs = base + "shaders/tests/rhitest_watershape.frag";

    struct V { float px, py, pz, nx, ny, nz; };

    // --- ANILLO CILINDRICO (un TORO): la normal barre TODAS las direcciones — hacia fuera, hacia
    //     dentro, arriba y abajo — asi que es la prueba dura de que el marco tangente se reconstruye
    //     bien en cualquier orientacion.
    //
    // ⚠️ La primera version era una BANDA cilindrica abierta y no valia: con `cull = None` se veia el
    // interior por las bocas y la silueta no se leia como un anillo. Un toro es cerrado y solo tiene
    // una lectura posible, que es lo que un test visual necesita.
    std::vector<V> ringV; std::vector<uint32_t> ringI;
    {
        const int major = 160, minor = 40;      // vueltas larga y corta
        const float R = 0.52f, r = 0.155f;      // radios mayor y menor
        for (int i = 0; i <= major; ++i) {
            const float a = 6.2831853f * (float)i / (float)major;
            const float ca = std::cos(a), sa = std::sin(a);
            for (int j = 0; j <= minor; ++j) {
                const float b = 6.2831853f * (float)j / (float)minor;
                const float cb = std::cos(b), sb = std::sin(b);
                // Normal del toro: radial respecto a la CIRCUNFERENCIA GUIA, no al centro.
                const glm::vec3 n(ca * cb, sb, sa * cb);
                const glm::vec3 p(ca * (R + r * cb), r * sb, sa * (R + r * cb));
                ringV.push_back({ p.x, p.y, p.z, n.x, n.y, n.z });
            }
        }
        const int stride = minor + 1;
        for (int i = 0; i < major; ++i)
            for (int j = 0; j < minor; ++j) {
                const uint32_t a = (uint32_t)(i * stride + j);
                for (uint32_t e : { 0u, 1u, (uint32_t)stride,
                                    1u, (uint32_t)(stride + 1), (uint32_t)stride })
                    ringI.push_back(a + e);
            }
    }

    // --- CUBO: `up` es la normal de cada cara, constante dentro de la cara y DISCONTINUA en la arista.
    std::vector<V> cubeV; std::vector<uint32_t> cubeI;
    {
        const float h = 0.42f;
        const glm::vec3 N[6] = {{0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0}};
        for (int f = 0; f < 6; ++f) {
            const glm::vec3 n = N[f];
            const glm::vec3 u = std::abs(n.y) > 0.9f ? glm::vec3(1,0,0) : glm::vec3(0,1,0);
            const glm::vec3 t = glm::normalize(glm::cross(u, n)), b = glm::cross(n, t);
            const uint32_t v0 = (uint32_t)cubeV.size();
            // Subdividido: sin vertices intermedios la ola no tendria donde desplazar nada.
            const int S = 24;
            for (int j = 0; j <= S; ++j)
                for (int i = 0; i <= S; ++i) {
                    const float fu = (float)i / S * 2.0f - 1.0f, fv = (float)j / S * 2.0f - 1.0f;
                    const glm::vec3 p = (n + t * fu + b * fv) * h;
                    cubeV.push_back({ p.x, p.y, p.z, n.x, n.y, n.z });
                }
            for (int j = 0; j < S; ++j)
                for (int i = 0; i < S; ++i) {
                    const uint32_t a = v0 + (uint32_t)(j * (S + 1) + i);
                    for (uint32_t e : { 0u, 1u, (uint32_t)(S+1), 1u, (uint32_t)(S+2), (uint32_t)(S+1) })
                        cubeI.push_back(a + e);
                }
        }
    }

    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides = { (uint32_t)sizeof(V) };
    pd.vertexLayout.attributes = {
        { 0, (uint32_t)offsetof(V, px), Format::RGB32F, 0 },
        { 1, (uint32_t)offsetof(V, nx), Format::RGB32F, 0 },
    };
    pd.depth.test = true; pd.depth.write = true; pd.depth.compare = CompareOp::Less;
    pd.blend.enable = false;
    pd.cull = CullMode::None;

    PipelineHandle pipe = g_dev->createPipeline(pd);
    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
    BufferHandle ocean = makeOceanStateUBO(st);
    CHECK(valid(pipe) && valid(ocean), "pipeline de formas de agua creado");
    if (!valid(pipe)) return;

    struct ShapeUBO { float vp[16]; float misc[4]; };
    // Camara fija mirando las dos piezas de tres cuartos. Ortografica: sin perspectiva la cobertura
    // en pixeles es comparable entre backends sin depender de la matriz de proyeccion.
    glm::mat4 proj = glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, -4.0f, 4.0f);
    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    (void)isVk;
    const glm::mat4 view = glm::lookAt(glm::vec3(1.15f, 0.95f, 1.3f), glm::vec3(0.0f), glm::vec3(0,1,0));

    // MAR EN CALMA: el mismo estado con las cuatro amplitudes a cero. Es la CONTRAPRUEBA del
    // contraste: sin ella, "la pieza tiene relieve" se cumpliria igual por el propio brillo especular
    // sobre un toro liso, y el test no estaria midiendo la ola sino la forma.
    // ⚠️ ESTE `4` ESCRITO A MANO DEJABA LA REFERENCIA "EN CALMA" CON OLA. Al subir el espectro de 4 a
    // 8 trenes, apagar solo los cuatro primeros deja los otros CUATRO a plena amplitud: el mar de
    // referencia contra el que se compara "con ola vs sin ola" seguia teniendo ola. Y como la medida
    // es una DIFERENCIA contra esa referencia, el resultado sale mas pequeño de lo que es — el fallo
    // no grita, susurra. Es el tercer `4` literal que destapa el mismo cambio (los otros dos, en el
    // UBO del banco, dieron agua invisible y un volcado de nucleo).
    Haruka::World::Water::OceanState calmSt = st;
    for (int i = 0; i < Haruka::World::Water::OCEAN_WAVES; ++i) calmSt.wave[i][1] = 0.0f;
    BufferHandle calm = makeOceanStateUBO(calmSt);

    struct Shape { const char* name; std::vector<V>* v; std::vector<uint32_t>* i; float depth; bool flat; };
    Shape shapes[] = {
        { "anillo, mar abierto (40 m)", &ringV, &ringI, 40.0f, false },
        { "anillo, rompiendo  (1,5 m)", &ringV, &ringI,  1.5f, false },
        { "anillo, EN CALMA (amp 0)",   &ringV, &ringI, 40.0f, true  },
        { "cubo,   mar abierto (40 m)", &cubeV, &cubeI, 40.0f, false },
        { "cubo,   rompiendo  (1,5 m)", &cubeV, &cubeI,  1.5f, false },
    };

    const int W = 256, H = 256;   // el de la ventana: ver la nota del viewport en testOceanWaveParity
    // ⚠️ EL NOMBRE LLEVABA EL CALMA AL MISMO FICHERO QUE EL HONDO. El anillo EN CALMA es tambien
    // `depth > 10`, asi que su PNG se llamaba igual que el de mar abierto y lo PISABA: quien fuera a
    // comparar "con ola contra en calma" —que es justo para lo que sirve el volcado— se encontraba la
    // misma imagen dos veces. Lo destapo el propio volcado, que imprimio `anillo_hondo` dos veces.
    auto shotBase = [&](const Shape& s2) {
        return std::string("agua_") + (s2.v == &ringV ? "anillo" : "cubo") +
               (s2.flat ? "_calma" : (s2.depth > 10.0f ? "_hondo" : "_rompiendo"));
    };
    long   cover[5] = { 0, 0, 0, 0, 0 };
    double contrast[5] = { 0, 0, 0, 0, 0 };
    double circul[5]   = { 0, 0, 0, 0, 0 };   // 4pi·area/perimetro²: 1,00 circulo · 0,785 cuadrado
    std::vector<std::vector<uint8_t>> frames(5);
    int    shapeIdx = 0;
    for (const Shape& sh : shapes) {
        BufferHandle vb = g_dev->createBuffer(BufferUsage::Vertex, sh.v->size()*sizeof(V), sh.v->data());
        BufferHandle ib = g_dev->createBuffer(BufferUsage::Index,  sh.i->size()*sizeof(uint32_t), sh.i->data());
        ShapeUBO su{};
        const glm::mat4 vp = proj * view;
        std::memcpy(su.vp, &vp[0][0], sizeof(su.vp));
        su.misc[0] = 9.5f;        // tiempo
        su.misc[1] = sh.depth;    // profundidad del agua
        // Metros por unidad de forma. Con 47 el toro mide 24,4 m de radio mayor, o sea 153 m de
        // vuelta: unas 2,5 longitudes de onda del tren dominante (61 m). Con el 22 de antes daba
        // 1,4 crestas — una sola ondulacion, que no deja ver si la ola recorre la pieza.
        su.misc[2] = 47.0f;
        BufferHandle ub = g_dev->createBuffer(BufferUsage::Uniform, sizeof(su), &su, BufferMemory::Dynamic);

        std::vector<uint8_t> px((size_t)W * H * 4, 0xAA);
        if (Context* c = g_dev->beginFrame()) {
            ClearValues cv;
            cv.clearColor = true; cv.color[0]=cv.color[1]=cv.color[2]=0.0f; cv.color[3]=1.0f;
            cv.clearDepth = true; cv.depth = 1.0f;   // test Less: el lejano es 1
            c->beginRenderPass({}, cv);
            c->setViewport(0, 0, W, H);
            c->bindPipeline(pipe);
            c->bindUniformBuffer(0, ub);
            c->bindUniformBuffer(29, sh.flat ? calm : ocean);
            c->bindVertexBuffer(vb);
            c->bindIndexBuffer(ib);
            c->drawIndexed((uint32_t)sh.i->size());
            c->endRenderPass();
            if (!isVk) g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk)  g_dev->readPixels(0, 0, W, H, Format::RGBA8, px.data());
        }

        // CONTRASTE de la pieza: desviacion tipica de la luminancia sobre los pixeles de agua. Es la
        // medida de "se ve relieve": una superficie lisa da un degradado suave (desviacion baja) y una
        // rizada enciende y apaga el especular (desviacion alta). Sustituye al conteo de espuma, que
        // no informaba de nada — a 1,5 m la espuma de ORILLA vale 1,0 en todo el cuadro por definicion.
        // ⚠️ CONTRASTE **LOCAL**, no la desviacion global. La global no servia y la contraprueba lo
        // demostro: el toro en calma daba MAS sigma (23,7) que el toro con olas (23,1), porque ese
        // numero lo domina el degradado suave del cuerpo —lado iluminado contra lado en sombra— y el
        // rizado, que es de alta frecuencia y poca amplitud, se pierde dentro. La diferencia entre
        // pixeles VECINOS ignora el degradado por construccion y solo ve el detalle fino.
        auto lum = [&](int x, int y) {
            const size_t k = ((size_t)y * W + x) * 4;
            return 0.2126 * px[k] + 0.7152 * px[k+1] + 0.0722 * px[k+2];
        };
        auto isWater = [&](int x, int y) {
            const size_t k = ((size_t)y * W + x) * 4;
            return px[k] + px[k+1] + px[k+2] > 24;
        };
        long lit = 0; double gradSum = 0.0; long gradN = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (!isWater(x, y)) continue;
                ++lit;
                // Solo pares interiores: en la silueta el salto al fondo negro es enorme y no es rizado.
                if (x + 1 < W && isWater(x + 1, y)) { gradSum += std::abs(lum(x+1,y) - lum(x,y)); ++gradN; }
                if (y + 1 < H && isWater(x, y + 1)) { gradSum += std::abs(lum(x,y+1) - lum(x,y)); ++gradN; }
            }
        const double sd = gradN ? gradSum / gradN : 0.0;
        // ⚠️ Y LA FORMA, QUE ESTE TEST NO SABIA VER. Andoni reporto que **en OpenGL el anillo se ve
        // como un cuadrado deformado** mientras en Vulkan sale redondo — y el test pasaba en verde en
        // los dos. El motivo: solo medía el AREA de la silueta ("18,7 %"), y un cuadrado y un circulo
        // de la misma area dan el mismo numero. Un test de forma que no mira la forma.
        //
        // La medida que si lo ve es la CIRCULARIDAD, `4π·area / perimetro²`: vale 1,00 para un
        // circulo perfecto, 0,785 para un cuadrado, y baja con cualquier deformacion. El perimetro se
        // cuenta como pixeles de agua con algun vecino de fondo (contorno de 4 vecinos).
        long peri = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (!isWater(x, y)) continue;
                const bool borde = (x == 0 || y == 0 || x == W-1 || y == H-1)
                                || !isWater(x-1, y) || !isWater(x+1, y)
                                || !isWater(x, y-1) || !isWater(x, y+1);
                if (borde) ++peri;
            }
        const double circ = (peri > 0) ? (4.0 * 3.14159265358979 * (double)lit / ((double)peri * (double)peri))
                                       : 0.0;
        std::printf("    %-28s %6ld px de agua (%.1f%%) · detalle fino (grad. local) %5.2f · "
                    "circularidad %.3f\n",
                    sh.name, lit, 100.0 * lit / (double)(W*H), sd, circ);
        CHECK(lit > (long)(W*H) / 50, "la forma se dibuja con agua encima");
        circul[shapeIdx] = circ;
        cover[shapeIdx] = lit; contrast[shapeIdx] = sd; frames[shapeIdx] = px; ++shapeIdx;

        // HARUKA_WATER_PNG=<dir> vuelca las cuatro formas a disco. El test AFIRMA que la silueta se
        // conserva, pero si la ola se ve bien o no es un juicio visual y hace falta poder mirarla.
        if (const char* dir = std::getenv("HARUKA_WATER_PNG")) {
            // readPixels de GL devuelve las filas de abajo arriba y las de Vulkan de arriba abajo:
            // sin voltear la de GL, los dos PNG saldrian espejados entre si.
            std::vector<uint8_t> img(px.size());
            for (int y = 0; y < H; ++y) {
                const int src = isVk ? y : (H - 1 - y);
                std::memcpy(&img[(size_t)y * W * 4], &px[(size_t)src * W * 4], (size_t)W * 4);
            }
            const std::string path = std::string(dir) + "/" + (isVk ? "vk_" : "gl_") +
                                     shotBase(sh) + ".png";
            Haruka::IO::writePNG(path, W, H, 4, img.data());
            std::printf("      -> %s\n", path.c_str());
        }

        // La captura entra en el careo GL<->Vulkan que hace compareBackends().
        recordShot(shotBase(sh).c_str(), W, H, px);

        g_dev->destroy(vb); g_dev->destroy(ib); g_dev->destroy(ub);
    }

    // ⚠️ La espuma NO demuestra que la cresta se pliegue, y decirlo seria pasarse. A 1,5 m el termino
    // de ESPUMA DE ORILLA (`shoreFoam`, que solo mira la profundidad) ya vale ~0,9 por si solo, asi
    // que satura el cuadro entero. Lo que este test demuestra es lo otro: que la ola se aplica sobre
    // un cilindro y sobre un cubo sin costuras ni huecos, o sea que NO necesita un planeta debajo.
    // El pliegue de verdad se mide en la CPU, sobre el jacobiano: `test_ocean_break_fold`.
    // ⚠️ LA SILUETA SE CONSERVA. Es la guardia contra el fallo que tenia la primera version: `disp`
    // sale en METROS y se sumaba a una posicion en unidades de forma, asi que la ola no rizaba la
    // pieza — la INFLABA. Medido entonces: el anillo pasaba del 16,6% al 48,0% de cobertura entre
    // mar abierto y rompiente, o sea que dejaba de ser un anillo. Con las unidades bien, la
    // profundidad cambia el RIZADO y no el tamano.
    const int pairs[2][2] = { { 0, 1 }, { 3, 4 } };
    for (int p = 0; p < 2; ++p) {
        const double a = (double)cover[pairs[p][0]], b = (double)cover[pairs[p][1]];
        const double drift = a > 0.0 ? 100.0 * std::abs(b - a) / a : 100.0;
        // ⚠️ LA FORMA, AFIRMADA. Un anillo cilindrico visto de frente es un disco: su circularidad tiene
    // que ser alta. Si la ola lo convierte en "un cuadrado deformado" —el reporte visual— esto lo ve,
    // y el area sola no lo veia.
    std::printf("    circularidad: anillo mar abierto %.3f · anillo rompiendo %.3f · anillo EN CALMA %.3f\n"
                "                  (1,00 = circulo · 0,785 = cuadrado · menos = deformado)\n",
                circul[0], circul[1], circul[2]);
    // ⚠️ NADA DE UMBRAL ABSOLUTO, Y AQUI ME EQUIVOQUE PRIMERO. Puse `circul > 0.55` esperando "un
    // disco" y el anillo EN CALMA da **0,472**: es un TORO, su silueta es una corona con agujero, o
    // sea DOS contornos, y eso hunde la circularidad por construccion. No estaba deformado — mi
    // referencia estaba mal. La pieza en calma es su propia referencia y es lo unico honesto que
    // comparar. (El cubo da 1,02, por encima del maximo geometrico de 1: el perimetro contado por
    // pixeles subestima en las diagonales. Sirve para COMPARAR formas entre si, no como valor absoluto.)
    CHECK(circul[2] > 0.2, "la pieza en calma tiene una silueta medible (referencia de forma)");
    // La ola RIZA el borde, asi que baja algo la circularidad — pero no puede convertir el disco en
    // otra figura. Se compara contra la pieza EN CALMA, que es su propia referencia.
    CHECK(circul[0] > circul[2] * 0.60,
          "con ola de MAR ABIERTO la silueta NO cambia de figura respecto a la pieza en calma");
    CHECK(circul[1] > circul[2] * 0.60,
          "y ROMPIENDO tampoco (es donde el adelanto de cresta es maximo)");
    std::printf("    %-6s: silueta %.1f%% -> %.1f%% al pasar de mar abierto a rompiente (deriva %.1f%%)\n",
                    p == 0 ? "anillo" : "cubo", 100.0 * a / (W*H), 100.0 * b / (W*H), drift);
        CHECK(drift < 10.0, "la ola riza la forma, no la infla (las unidades del desplazamiento casan)");
    }

    // ⚠️ LA OLA SOBRE UNA PIEZA DE ESTE TAMANO ES SUTIL, Y CONVIENE DECIRLO CON NUMEROS EN VEZ DE
    // PROMETER LO CONTRARIO. Se intento afirmar "la ola se VE" por contraste y no se sostiene: con
    // olas el detalle fino da 1,81 y en calma 1,74 — un 4%. El motivo es geometrico y no un fallo:
    // la pieza abarca +-31,7 m, o sea UNA longitud de onda del tren dominante (61 m), asi que la ola
    // no la riza sino que la INCLINA entera, y una inclinacion uniforme no genera detalle local. (El
    // arco del toro no cuenta: la ola es un plano en 3D, lo que importa es la extension LINEAL.)
    // Ademas el oleaje real es tendido: con H/lambda ~ 5,6%, cualquier vista que abarque varias
    // longitudes de onda muestra ondulaciones del 5%. El mar es asi.
    //
    // Lo que SI se puede afirmar sin adornos es que la ola hace algo, y se mide comparando el mismo
    // toro con olas y en calma pixel a pixel. Es mas sensible que cualquier estadistico de contraste
    // y no es tautologico: calma y oleaje son entradas distintas de verdad.
    {
        const std::vector<uint8_t>& A = frames[0];   // anillo con olas
        const std::vector<uint8_t>& B = frames[2];   // el mismo anillo en calma
        long diff = 0, both = 0; double sumAbs = 0.0; int worst = 0;
        for (size_t k = 0; k + 3 < A.size() && k + 3 < B.size(); k += 4) {
            const bool wa = A[k] + A[k+1] + A[k+2] > 24;
            const bool wb = B[k] + B[k+1] + B[k+2] > 24;
            if (!wa && !wb) continue;
            ++both;
            int mx = 0;
            for (int ch = 0; ch < 3; ++ch) {
                const int d = std::abs((int)A[k+ch] - (int)B[k+ch]);
                sumAbs += d; mx = std::max(mx, d);
            }
            worst = std::max(worst, mx);
            if (mx > 8) ++diff;
        }
        std::printf("    con olas vs EN CALMA (el mismo anillo): %.1f%% de pixeles distintos · |delta| medio %.2f · peor canal %d\n",
                    both ? 100.0 * diff / both : 0.0, both ? sumAbs / (both * 3) : 0.0, worst);
        std::printf("      (detalle fino: con olas %.2f · en calma %.2f — la ola INCLINA la pieza mas que rizarla,\n"
                    "       porque abarca ~1 longitud de onda; para verla rizada hace falta una superficie de varias)\n",
                    contrast[0], contrast[2]);
        CHECK(both > 1000, "hay pieza que comparar entre las dos versiones");
        CHECK(diff > both / 20, "el oleaje cambia la pieza respecto al mar en calma (la ola hace algo)");
    }

    std::printf("    (la espuma a 1,5 m es la de ORILLA, que solo mira la profundidad: no prueba pliegue)\n");
    g_dev->destroy(pipe); g_dev->destroy(ocean); g_dev->destroy(calm);
}


// ================================================================================================
// EL BAKE CONTRA LA CPU, NIVEL A NIVEL. ¿La divergencia entre GPUs escala con el nodo?
//
// ⚠️ EL BANCO MIRABA UN SOLO NODO, Y DEL NIVEL 17. Ahi la divergencia AMD/NVIDIA es de 2 cm
// (0,0204 contra 0,0001 m), y Andoni ve 0,4-1,5 m en partida — donde se dibujan niveles 9..17. Si el
// ruido diverge mas cuanto mayor es la coordenada, los nodos GRUESOS serian mucho peores y una sola
// muestra del 17 no lo veria. Esto barre el nivel para contestarlo.
//
// No dibuja nada: genera en GPU, lee el heightmap y lo compara con `nodeFillHeights`, que es el
// gemelo CPU. La referencia NO depende de la GPU, asi que la comparacion es honesta en las dos.
// ================================================================================================
// ================================================================================================
// ¿DA LA GPU EL MISMO NUMERO DE ONDA QUE LA CPU? El eslabon que `rhitest_waveprobe` no aisla.
//
// La sonda de la ola compara ALTURAS, o sea el final de la cadena: cuando dio 4,6 m de separacion al
// meter la dispersion de profundidad finita, no decia si el culpable era `k`, el desvanecido de onda
// corta, el tope de rompiente o la fase. Esto emite SOLO `harukaWaveNumber`.
//
// Y emite ademas el RESIDUO de la propia ecuacion de dispersion, que separa dos casos que a simple
// vista son el mismo: "las dos resuelven bien y difieren en el ultimo bit" (residuo ~0 en ambas) de
// "el solver de la GPU no converge" (residuo grande en una).
// ================================================================================================
// ================================================================================================
// EL CAMPO DE AGUA HORNEADO: ¿lee la GPU el mismo lago que la CPU?
//
// ⚠️ TODO ESTE CAMINO SE AÑADIÓ SIN UNA SOLA PRUEBA DE GPU. `bakeWaterMap` rellena las cuencas del
// planeta, sube el resultado como RG32F (R = cota, G = fetch) y el shader lo lee con
// `harukaBakedLakeAt` / `harukaBakedFetchAt`; la CPU tiene `lakeLevelAt` / `lakeFetchAt`, declaradas
// gemelas y nunca comparadas. Si divergen, el agua que se DIBUJA no está donde la física dice.
//
// El campo NO se toma de un planeta: se construye aquí una altura sintética, se pasa por el MISMO
// `waterFillEquirect` que usa el motor y se sube. Así el test audita el par textura↔lector, que es
// lo que no estaba probado, sin depender de que haya un mundo horneado.
// ================================================================================================
void testWaterFieldParity()
{
    BEGIN("agua: el campo de lagos de la GPU == el de la CPU");

    // ── EL CAMPO: un continente con una cuenca cerrada, como el test de CPU ─────────────────────
    const int W = 128, H = 64;
    std::vector<float> land((size_t)W * H, 100.0f);
    auto at = [&](int x, int y) -> float& { return land[(size_t)y * W + x]; };
    for (int y = 0; y < H; ++y) for (int x = 0; x < 12; ++x) at(x, y) = -500.0f;   // océano
    for (int y = 24; y < 40; ++y) for (int x = 60; x < 76; ++x) at(x, y) = 10.0f;  // cuenca
    // ⚠️ RADIO PEQUENO A PROPOSITO. Con el radio terrestre, un lago de 16x16 texeles sobre un mapa
    // de 128x64 mide MILES de kilometros y su factor de fetch satura en 1,0 — o sea que el careo del
    // fetch compararia 1,0 contra 1,0 y no diria nada. Con un cuerpo pequeno el lago sale de cientos
    // de metros, el factor cae por debajo de 1 y la comparacion mide algo.
    const double kProbeRadius = 4000.0;
    const Haruka::World::Water::WaterFillResult fill =
        Haruka::World::Water::waterFillEquirect(land.data(), W, H, 0.0f, kProbeRadius);
    std::printf("    campo sintetico %dx%d: %zu texeles de mar · %zu de lago · fetch mayor %.0f m\n",
                W, H, fill.seaCells, fill.lakeCells, fill.biggestFetchM);
    CHECK(fill.lakeCells > 0, "el campo de prueba TIENE lago (si no, el careo no compara nada)");

    // Intercalado RG, igual que `bakeWaterMap`.
    std::vector<float> rg((size_t)W * H * 2);
    for (size_t c = 0; c < (size_t)W * H; ++c) {
        rg[c * 2 + 0] = fill.levelM[c];
        rg[c * 2 + 1] = fill.fetchM[c];
    }
    TextureDesc td;
    td.width = (uint32_t)W; td.height = (uint32_t)H;
    td.format = Format::RG32F; td.filter = Filter::Nearest; td.wrap = Wrap::ClampToEdge;
    td.mipmaps = false; td.initialData = rg.data();
    TextureHandle lakeTex = g_dev->createTexture(td);
    CHECK(valid(lakeTex), "textura RG32F del campo de agua creada");
    if (!valid(lakeTex)) return;

    // ── LAS DIRECCIONES A PROBAR: dentro del lago, en el mar y en tierra ────────────────────────
    //
    // El muestreo es NEAREST y la inversa de `equirectUV`, así que se eligen CENTROS DE TÉXEL: un
    // punto en el borde caería en un téxel u otro según el último bit y el careo mediría eso.
    auto dirOfTexel = [&](int x, int y) {
        const double u = ((double)x + 0.5) / W, v = ((double)y + 0.5) / H;
        const double lat = (0.5 - v) * 3.14159265358979323846;
        const double lon = (u - 0.5) * 2.0 * 3.14159265358979323846;
        const double cl = std::cos(lat);
        return glm::dvec3(cl * std::cos(lon), std::sin(lat), cl * std::sin(lon));
    };
    struct Probe { int x, y; const char* what; };
    const Probe probes[6] = { { 68, 32, "centro del lago" }, { 61, 25, "borde del lago" },
                              {  4, 32, "oceano" },          { 40, 10, "tierra seca" },
                              { 120, 50, "tierra, otra cara" }, { 75, 39, "esquina del lago" } };
    const int N = 6;
    const int kOut = 5;   // gemelo de `PROBE_OUT` en water_field_probe.comp
    std::vector<float> inBuf(1 + (size_t)N * 3);
    inBuf[0] = (float)N;
    for (int k = 0; k < N; ++k) {
        const glm::dvec3 d = dirOfTexel(probes[k].x, probes[k].y);
        inBuf[1 + k * 3 + 0] = (float)d.x;
        inBuf[1 + k * 3 + 1] = (float)d.y;
        inBuf[1 + k * 3 + 2] = (float)d.z;
    }

    // ── LA VENTANA FINA, tambien de verdad ──────────────────────────────────────────────────────
    //
    // ⚠️ Y NO ES OPCIONAL ATARLA. Desde que la sonda declara el binding 19 y el UBO 26, dejarlos
    // sueltos en Vulkan es comportamiento INDEFINIDO — no ceros. Es exactamente lo que ya dejo el
    // agua del pase de nodos sin dibujar un pixel.
    //
    // Se centra en el lago del campo grueso, para que el careo tenga la ventana MOJADA en unos puntos
    // y seca en otros.
    const glm::dvec3 dirLago = dirOfTexel(68, 32);
    double lonW = 0.0, latW = 0.0;
    Haruka::World::Water::waterDirToLonLat(dirLago, lonW, latW);
    const double halfLatW = 400.0 / kProbeRadius;                 // ±400 m en un cuerpo de 4 km
    const double halfLonW = halfLatW / std::max(std::cos(latW), 1e-6);
    const double invLonW = 1.0 / (2.0 * halfLonW), invLatW = 1.0 / (2.0 * halfLatW);
    auto dirOfLL = [](double lo, double la) {
        return glm::dvec3(std::cos(la) * std::cos(lo), std::sin(la), std::cos(la) * std::sin(lo));
    };
    // Terreno fino sintetico: un cuenco de 200 m dentro del lago grueso.
    auto terrenoW = [&](double lo, double la) -> float {
        const glm::dvec3 d = dirOfLL(lo, la);
        const double dM = std::acos(glm::clamp(glm::dot(d, dirLago), -1.0, 1.0)) * kProbeRadius;
        return (dM < 200.0) ? (float)(5.0 - 4.0 * (1.0 - (dM / 200.0) * (dM / 200.0))) : 5.0f;
    };
    auto contornoW = [&](double, double) -> float { return Haruka::World::Water::WATER_FILL_DRY; };
    const Haruka::World::Water::WaterWindowResult win =
        Haruka::World::Water::waterFillWindow(64, 64, lonW, latW, halfLonW, halfLatW, kProbeRadius,
                                        terrenoW, contornoW, 0.0f);
    std::printf("    ventana fina 64x64 (%.1f m/texel): %zu texeles de lago\n",
                win.texelM, win.lakeCells);
    CHECK(win.lakeCells > 0, "la ventana de prueba TIENE lago (si no, su careo no compara nada)");

    std::vector<float> rgw((size_t)win.w * win.h * 2);
    for (size_t c = 0; c < (size_t)win.w * win.h; ++c) {
        rgw[c * 2 + 0] = win.levelM[c];
        rgw[c * 2 + 1] = win.fetchM[c];
    }
    TextureDesc twd;
    twd.width = (uint32_t)win.w; twd.height = (uint32_t)win.h;
    twd.format = Format::RG32F; twd.filter = Filter::Nearest; twd.wrap = Wrap::ClampToEdge;
    twd.mipmaps = false; twd.initialData = rgw.data();
    TextureHandle winTex = g_dev->createTexture(twd);
    const float rect[4] = { (float)lonW, (float)latW, (float)invLonW, (float)invLatW };
    BufferHandle winUBO = g_dev->createBuffer(BufferUsage::Uniform, sizeof(rect), rect,
                                              BufferMemory::Static);

    const std::string cs = Haruka::Renderer::Shader::baseDir() + "shaders/tests/water_field_probe.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de water_field_probe.comp creado");
    if (!valid(cp)) { g_dev->destroy(lakeTex); return; }

    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, (size_t)N * kOut * sizeof(float),
                                           nullptr, BufferMemory::Static);
    BufferHandle rb  = g_dev->createBuffer(BufferUsage::Storage, (size_t)N * kOut * sizeof(float),
                                           nullptr, BufferMemory::Readback);
    BufferHandle in  = g_dev->createBuffer(BufferUsage::Storage, inBuf.size() * sizeof(float),
                                           inBuf.data(), BufferMemory::Static);
    const Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
    BufferHandle ocean = makeOceanStateUBO(st);
    if (Context* ctx = g_dev->beginFrame()) {
        ctx->bindPipeline(cp);
        ctx->bindStorageBuffer(0, out);
        ctx->bindStorageBuffer(1, in);
        ctx->bindTexture(18, lakeTex);
        ctx->bindTexture(19, winTex);
        ctx->bindUniformBuffer(26, winUBO);
        if (valid(ocean)) ctx->bindUniformBuffer(29, ocean);
        ctx->dispatch(1, 1, 1);
        ctx->memoryBarrier();
        g_dev->endFrame();
    }
    copyThenWait(out, rb, 0, (size_t)N * kOut * sizeof(float));
    const float* v = (const float*)g_dev->mappedData(rb);
    if (!v) { CHECK(false, "readback"); g_dev->destroy(rb); g_dev->destroy(out);
              g_dev->destroy(in); g_dev->destroy(cp); g_dev->destroy(lakeTex); return; }

    std::printf("    punto                  cota GPU     cota CPU      fetch GPU    fetch CPU   factor\n");
    double worstLvl = 0.0, worstFetch = 0.0, worstFac = 0.0;
    bool sawLake = false, sawDry = false;
    for (int k = 0; k < N; ++k) {
        const size_t c = (size_t)probes[k].y * W + probes[k].x;
        // El gemelo de CPU: el MISMO muestreo NEAREST que `TerrestrialPlanet::lakeLevelAt`.
        const float lvlCpu = fill.levelM[c];
        const float fchCpu = (fill.fetchM[c] > 0.0f) ? fill.fetchM[c]
                                                     : Haruka::World::Water::WATER_FETCH_UNLIMITED;
        const float facCpu = Haruka::World::Water::oceanFetchFactor(st, fchCpu);
        // El shader devuelve su propio centinela donde no hay lago: se comparan como "no hay".
        const bool gpuDry = (v[k * kOut + 0] < -1.0e8f);
        const bool cpuDry = (lvlCpu <= Haruka::World::Water::WATER_FILL_DRY);
        if (!cpuDry) sawLake = true; else sawDry = true;
        std::printf("    %-20s %11.3f %12.3f %13.0f %12.0f %8.4f\n", probes[k].what,
                    gpuDry ? 0.0 : (double)v[k * kOut + 0], cpuDry ? 0.0 : (double)lvlCpu,
                    (double)v[k * kOut + 1], (double)fchCpu, (double)v[k * kOut + 2]);
        CHECK(gpuDry == cpuDry, "GPU y CPU coinciden en SI hay lago en ese punto");
        if (!cpuDry) worstLvl = std::max(worstLvl, std::fabs((double)v[k*kOut+0] - (double)lvlCpu));
        worstFetch = std::max(worstFetch, std::fabs((double)v[k*kOut+1] - (double)fchCpu)
                                        / std::max(1.0, (double)fchCpu));
        worstFac   = std::max(worstFac,   std::fabs((double)v[k*kOut+2] - (double)facCpu));
    }
    std::printf("    peor diferencia: cota %.6f m · fetch %.3e relativo · factor de ola %.6f\n",
                worstLvl, worstFetch, worstFac);

    CHECK(sawLake && sawDry,
          "el careo mira puntos con lago Y sin el (si no, seria tautologico)");
    CHECK(worstLvl < 1e-3, "la COTA de la lamina es la misma en la GPU que en la CPU");
    CHECK(worstFetch < 1e-5, "y el FETCH tambien");
    CHECK(worstFac < 1e-4, "y con el, el factor que decide la ola que se dibuja en ese lago");
    // ⚠️ Y QUE EL FACTOR NO ESTE SATURADO: con 1,0 en todos los puntos, la linea de arriba compararia
    // 1,0 contra 1,0. La primera version de este test hacia exactamente eso.
    double facLake = 1.0;
    for (int k = 0; k < N; ++k) {
        const size_t c = (size_t)probes[k].y * W + probes[k].x;
        if (fill.levelM[c] > Haruka::World::Water::WATER_FILL_DRY)
            facLake = std::min(facLake, (double)v[k * kOut + 2]);
    }
    std::printf("    factor de fetch en el lago: %.4f (si fuera 1,0 el careo del fetch no mediria nada)\n",
                facLake);
    CHECK(facLake < 0.9, "CONTRAPRUEBA: el fetch del lago LIMITA la ola de verdad (factor < 1)");

    // ── Y LA VENTANA FINA: el otro sitio donde CPU y GPU pueden separarse ───────────────────────
    //
    // Son dos convenciones distintas (equirect global contra recuadro lon/lat con desenvuelto de la
    // longitud), asi que casar el campo global NO dice nada de esta. `waterWindowLevelAt` de
    // `WaterFill.h` es la unica implementacion de CPU y `harukaLakeWinUV` su gemelo declarado.
    std::printf("    punto                  ventana GPU  ventana CPU   fetch comb GPU  fetch comb CPU\n");
    double worstWin = 0.0, worstComb = 0.0;
    int mojados = 0, secos = 0;
    for (int k = 0; k < N; ++k) {
        const glm::dvec3 d = dirOfTexel(probes[k].x, probes[k].y);
        const float winCpu = Haruka::World::Water::waterWindowLevelAt(win, d, lonW, latW, invLonW, invLatW);
        const bool cpuDry = (winCpu <= Haruka::World::Water::WATER_FILL_DRY);
        const bool gpuDry = (v[k * kOut + 3] < -1.0e8f);
        if (cpuDry) ++secos; else ++mojados;
        // El fetch COMBINADO: la ventana manda donde ve lamina, el global si no.
        const size_t c = (size_t)probes[k].y * W + probes[k].x;
        float combCpu;
        if (!cpuDry) combCpu = Haruka::World::Water::waterWindowFetchAt(win, d, lonW, latW, invLonW, invLatW);
        else         combCpu = (fill.fetchM[c] > 0.0f) ? fill.fetchM[c]
                                                       : Haruka::World::Water::WATER_FETCH_UNLIMITED;
        std::printf("    %-20s %12.3f %12.3f %15.0f %15.0f\n", probes[k].what,
                    gpuDry ? 0.0 : (double)v[k * kOut + 3], cpuDry ? 0.0 : (double)winCpu,
                    (double)v[k * kOut + 4], (double)combCpu);
        CHECK(gpuDry == cpuDry, "GPU y CPU coinciden en si la VENTANA moja ese punto");
        if (!cpuDry) worstWin = std::max(worstWin, std::fabs((double)v[k*kOut+3] - (double)winCpu));
        worstComb = std::max(worstComb, std::fabs((double)v[k*kOut+4] - (double)combCpu)
                                       / std::max(1.0, (double)combCpu));
    }
    std::printf("    peor diferencia en la ventana: cota %.6f m · fetch combinado %.3e relativo\n",
                worstWin, worstComb);
    CHECK(mojados > 0 && secos > 0,
          "el careo de la ventana mira puntos DENTRO y FUERA de ella (si no, seria tautologico)");
    CHECK(worstWin < 1e-3, "la cota de la VENTANA es la misma en la GPU que en la CPU");
    CHECK(worstComb < 1e-5, "y el fetch combinado tambien (es el que entra en la Gerstner)");

    g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(in);
    g_dev->destroy(cp); g_dev->destroy(lakeTex);
    g_dev->destroy(winTex); g_dev->destroy(winUBO);
}


void testWaveNumberParity()
{
    BEGIN("mar: el numero de onda de la GPU == el de la CPU");

    const std::string cs = Haruka::Renderer::Shader::baseDir() + "shaders/tests/wavenumber_probe.comp";
    PipelineDesc pd; pd.computePath = cs.c_str();
    PipelineHandle cp = g_dev->createPipeline(pd);
    CHECK(valid(cp), "pipeline de wavenumber_probe.comp creado");
    if (!valid(cp)) return;

    const float depths[12] = { 0.05f, 0.1f, 0.3f, 0.5f, 1.0f, 2.0f,
                               3.0f, 5.18f, 10.0f, 23.45f, 100.0f, 400.0f };
    const float k0 = 6.2831853f / Haruka::World::Water::OCEAN_WAVE[0][0];
    // El MISMO punto y tiempo que usa `rhitest_waveprobe` en su peor pixel, para poder carear los dos
    // caminos: si la altura casa aqui (compute) y no alli (fragmento), el fallo es de aquel camino.
    const glm::vec3 wpProbe = glm::vec3(1000.0f, 0.0f, -500.0f)
                            + glm::vec3(0.7f, 0.0f, 0.11f) * 133.0f
                            + glm::vec3(0.13f, 0.0f, 0.9f) * 52.0f;
    const glm::vec3 upProbe(0.0f, 1.0f, 0.0f);
    const float     tProbe = 12.75f;
    float inputs[20]; inputs[0] = k0;
    for (int i = 0; i < 12; ++i) inputs[1 + i] = depths[i];
    inputs[13] = wpProbe.x; inputs[14] = wpProbe.y; inputs[15] = wpProbe.z;
    inputs[16] = upProbe.x; inputs[17] = upProbe.y; inputs[18] = upProbe.z;
    inputs[19] = tProbe;

    const size_t N = 28;
    BufferHandle out = g_dev->createBuffer(BufferUsage::Storage, N * sizeof(float), nullptr,
                                           BufferMemory::Static);
    BufferHandle rb  = g_dev->createBuffer(BufferUsage::Storage, N * sizeof(float), nullptr,
                                           BufferMemory::Readback);
    BufferHandle in  = g_dev->createBuffer(BufferUsage::Storage, sizeof(inputs), inputs,
                                           BufferMemory::Static);
    if (Context* ctx = g_dev->beginFrame()) {
        ctx->bindPipeline(cp);
        ctx->bindStorageBuffer(0, out);
        ctx->bindStorageBuffer(1, in);
        ctx->dispatch(1, 1, 1);
        ctx->memoryBarrier();
        g_dev->endFrame();
    }
    copyThenWait(out, rb, 0, N * sizeof(float));
    const float* v = (const float*)g_dev->mappedData(rb);
    if (!v) { CHECK(false, "readback"); g_dev->destroy(rb); g_dev->destroy(out);
              g_dev->destroy(in); g_dev->destroy(cp); return; }

    std::printf("    tren de %.1f m (k0 = %.6f)\n", Haruka::World::Water::OCEAN_WAVE[0][0], k0);
    std::printf("    prof.      k GPU      k CPU     dif.rel    residuo GPU  residuo CPU\n");
    double worstRel = 0.0, worstResGpu = 0.0;
    for (int i = 0; i < 12; ++i) {
        const float kc = Haruka::World::Water::oceanWaveNumber(k0, depths[i]);
        const double rel = std::fabs((double)v[i] - (double)kc) / std::max((double)kc, 1e-9);
        const double resC = ((double)(Haruka::World::Water::OCEAN_G * kc * std::tanh(kc * depths[i]))
                           - (double)(Haruka::World::Water::OCEAN_G * k0))
                          / (double)(Haruka::World::Water::OCEAN_G * k0);
        worstRel = std::max(worstRel, rel);
        worstResGpu = std::max(worstResGpu, std::fabs((double)v[12 + i]));
        std::printf("    %7.2f m %10.6f %10.6f  %9.2e  %11.2e %11.2e\n",
                    depths[i], v[i], kc, rel, (double)v[12 + i], resC);
    }
    std::printf("    peor diferencia relativa GPU<->CPU: %.3e · peor residuo de la GPU: %.3e\n",
                worstRel, worstResGpu);

    // ── LA ALTURA, EN COMPUTE, EN EL PEOR PUNTO DE LA OTRA SONDA ────────────────────────────────
    const Haruka::World::Water::OceanState stRef = Haruka::World::Water::oceanDefaultState();
    const float hCpu = Haruka::World::Water::oceanWaveHeight(wpProbe, upProbe, tProbe, depths[7], 1.0f, stRef);
    const float k0f  = 6.2831853f / stRef.wave[0][0];
    std::printf("    altura a %.2f m de fondo: GPU %+.6f m · CPU %+.6f m · dif %.6f m\n",
                depths[7], v[24], hCpu, std::fabs(v[24] - hCpu));
    std::printf("      piezas   green GPU %.6f / CPU %.6f · breakScale GPU %.6f / CPU %.6f\n",
                v[25], Haruka::World::Water::oceanGreenGain(depths[7]),
                v[26], Haruka::World::Water::oceanBreakScale(depths[7], stRef));
    std::printf("      fade del tren largo: GPU %.6f / CPU %.6f\n",
                v[27], Haruka::World::Water::oceanShortWaveFade(
                           Haruka::World::Water::oceanWaveNumber(k0f, depths[7])));
    CHECK(std::fabs(v[24] - hCpu) < 1e-3f,
          "la altura de la ola casa GPU<->CPU calculada en COMPUTE (aisla el camino del fragmento)");

    // ⚠️ EL UMBRAL SALE DE LA FASE, NO DE UN GUSTO. La fase es `k · dot(D,wp)`, y en la sonda de la
    // ola `dot(D,wp)` llega a ~1100 m: una diferencia relativa de 1e-4 en `k` ya son 0,018 rad de
    // fase, y 1e-2 descorrelaciona las dos olas por completo. Se pide 1e-5.
    CHECK(worstRel < 1e-5, "la GPU resuelve el MISMO numero de onda que la CPU");
    // Y el oraculo independiente: la GPU satisface la ecuacion que dice resolver.
    CHECK(worstResGpu < 1e-4, "y el `k` de la GPU satisface la relacion de dispersion");
    g_dev->destroy(rb); g_dev->destroy(out); g_dev->destroy(in); g_dev->destroy(cp);
}

void testNodeWaterDraws()
{
    BEGIN("agua: el pase de nodos DIBUJA agua (y la descarta en tierra)");

    using namespace Haruka::World::Terrain;
    const double R = 6371000.0;
    const glm::dvec3 pc(0.0);

    Haruka::World::Terrain::TerrainNodeRenderer r;
    if (!r.init(g_dev, Haruka::Renderer::Shader::baseDir() + "shaders/", 256)) {
        CHECK(false, "init del pase"); return;
    }
    uint32_t uw = 0, uh = 0; g_dev->framebufferSize(uw, uh);
    const int w = (uw > 0) ? (int)uw : 256, h = (uh > 0) ? (int)uh : 256;
    const double fovY = 60.0 * 3.14159265358979 / 180.0;
    const double radPerPx = fovY / (double)h;
    const double cone = Haruka::World::Terrain::nodeFrustumConeHalfAngle(fovY, (double)w / (double)h);
    const glm::dvec3 up0 = glm::normalize(glm::dvec3(1.0, 0.05, 0.03));
    const bool isVk = (g_dev->backend() == Backend::Vulkan);

    // Un "bake" de altura sintetico de 4x2: constante, asi que la bilineal de `harukaSampleHeightField`
    // devuelve ese valor caiga donde caiga y la profundidad es la misma en toda la esfera.
    auto makeFloor = [&](float metres) {
        std::vector<float> d(8, metres);
        TextureDesc td; td.width = 4; td.height = 2; td.format = Format::R32F;
        td.filter = Filter::Linear; td.wrap = Wrap::ClampToEdge; td.mipmaps = false;
        td.initialData = d.data();
        return g_dev->createTexture(td);
    };
    // ⚠️ EL MAR, POR ENCIMA DEL RELIEVE. Con la cota 0 el agua queda al nivel MEDIO del terreno
    // procedural (+-900 m), y mirando desde arriba se ve siempre la superficie MAS ALTA de cada rayo
    // — que esta sesgada hacia los picos. El test daba 0,6 % y parecia que el pase no dibujaba;
    // apagando el test de profundidad salia 100 %, o sea que la geometria estaba bien y la escena
    // mal. Con la lamina a +2000 m el agua tapa el relieve y lo que se mide es lo que se queria medir.
    Haruka::World::Water::OceanState st = Haruka::World::Water::oceanDefaultState();
    st.seaLevelM = 2000.0f;
    BufferHandle oceanUBO = makeOceanStateUBO(st);

    auto waterPixels = [&](float floorM, size_t& sentinelOut) {
        TextureHandle floorTex = makeFloor(floorM);
        Haruka::World::Terrain::TerrainNodeRenderer::Water wcfg;
        wcfg.heightTex   = floorTex;
        wcfg.oceanParams = oceanUBO;
        wcfg.on = true;
        r.setWater(wcfg);

        // ⚠️ AL NADIR Y DESDE ARRIBA, NO AL HORIZONTE. La primera version miraba a la tangente desde
        // 30 m: a esa altura el relieve procedural (+-900 m) TAPA el agua al nivel del mar en casi
        // toda la pantalla, y el test daba 0 px acusando al pase de no dibujar cuando lo que fallaba
        // era la camara. Mirando abajo desde 5 km se ven las depresiones, que es donde hay agua.
        const glm::dvec3 cam = pc + up0 * (R + 5000.0);
        const glm::dvec3 fwd = -up0;
        const glm::dvec3 vup = glm::normalize(glm::cross(fwd, glm::dvec3(0, 1, 0)));
        // ⚠️ REVERSED-Z, Y AQUI ESTABA EL FALLO — DEL TEST, NO DEL PASE. `glm::perspective` da una
        // proyeccion NORMAL (cerca->0, lejos->1) y el motor compara con `Greater`, que es la
        // convencion de reversed-Z: con la proyeccion normal, `Greater` se queda con lo MAS LEJANO.
        // El agua salia a 3 294 m y el terreno a 4 100-5 900, asi que ganaba el terreno y el test
        // daba 0 px acusando al pase de no dibujar. `testTerrainNodeCoverage` no lo nota porque solo
        // cuenta cobertura, y le da igual cual de las dos superficies gane.
        //
        // Reversed-Z con plano lejano infinito: z_ndc = near/dist, o sea 1 en el plano cercano y 0 en
        // el infinito. Es la que emite `Camera::getProjectionMatrix` en el motor.
        const float aspect = (float)w / (float)h;
        const float fCot   = 1.0f / std::tan((float)fovY * 0.5f);
        glm::mat4 proj(0.0f);
        proj[0][0] = fCot / aspect; proj[1][1] = fCot;
        proj[2][3] = -1.0f;         proj[3][2] = 1.0f;   // near = 1 m
        const glm::mat4 view = glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup));
        const glm::mat4 mvp  = proj * glm::mat4(glm::mat3(view));

        std::vector<uint8_t> px((size_t)w * h * 4, 0xAA);
        for (int f = 0; f < 12; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f;
            cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame();
            pumpWindowEvents();
            if (isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
        // AZUL: el agua sombrea con `harukaOceanShade`, cuyo canal B domina sobre R en toda su
        // paleta. El terreno del mismo pase no lo hace. Es el discriminante que separa "hay agua"
        // de "hay algo dibujado".
        // ⚠️ EL DISCRIMINANTE SE MIDE, NO SE ADIVINA. Este test ya fallo TRES veces por el
        // instrumento y no por el pase: contando magenta con una regla que exigia B > R, mirando al
        // horizonte donde el relieve tapa el agua, y con un umbral de azul elegido a ojo. Ahora se
        // imprime el color medio de lo dibujado, asi que si vuelve a dar 0 se ve POR QUE.
        size_t blue = 0, sentinel = 0, lit = 0;
        double sr = 0.0, sg = 0.0, sb = 0.0;
        for (size_t k = 0; k < px.size(); k += 4) {
            if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) { ++sentinel; continue; }
            if (px[k] || px[k+1] || px[k+2]) { ++lit; sr += px[k]; sg += px[k+1]; sb += px[k+2]; }
            if (px[k+2] > px[k] + 4 && px[k+2] > 8) ++blue;
        }
        std::printf("      (fondo %+.0f m) pixeles con color %zu · medio RGB %.0f/%.0f/%.0f\n",
                    (double)floorM, lit, lit ? sr / lit : 0.0, lit ? sg / lit : 0.0,
                    lit ? sb / lit : 0.0);
        sentinelOut = sentinel;
        g_dev->destroy(floorTex);
        return blue;
    };

    // El fondo sintetico decide la PROFUNDIDAD, que es lo unico que separa agua de tierra:
    //   -500 m -> profundidad 2500 m: hay mar.
    //   +5000 m -> profundidad -3000 m: tierra seca, el fragmento descarta.
    // La lamina esta en los dos casos a la MISMA cota, asi que lo que cambia es el descarte y no la
    // oclusion — que es justo lo que la contraprueba tiene que aislar.
    size_t sSea = 0, sLand = 0;
    const size_t blueSea  = waterPixels(-500.0f,  sSea);
    const size_t blueLand = waterPixels(+5000.0f, sLand);
    const size_t total = (size_t)w * h;
    std::printf("    con MAR (fondo -500 m): %zu px de agua (%.1f %%) · centinela %zu\n",
                blueSea, 100.0 * (double)blueSea / (double)total, sSea);
    std::printf("    con TIERRA (fondo +5000 m): %zu px de agua (%.1f %%) · centinela %zu\n",
                blueLand, 100.0 * (double)blueLand / (double)total, sLand);

    // ── ⚠️ LA OLA VIAJA EN EL PASE REAL (2026-09-18) ────────────────────────────────────────────
    //
    // La fase `k·(D·x) − ω·t` se evaluaba con `x = dir·R`: `D` tangente, `x` radial, producto CERO.
    // El mar entero subia en bloque y las "olas" eran ruido de float. NINGUN test lo veia: los careos
    // CPU<->GPU usan marco plano, y este test solo contaba pixeles azules. Ahora la ola se evalua
    // relativa al ANCLA del mar (`uOceanAnchor`), y aqui se mide en el pase de nodos de verdad: a
    // reloj fijo, la sombra del mar tiene que VARIAR por la imagen (la normal analitica sigue a la
    // ola). CONTRAPRUEBA: con el ancla en el centro del planeta (`anchorRelEye = −cam`, lo que hacia
    // el vert) la sombra es plana.
    {
        auto shadeSigma = [&](const glm::vec3& anchorRelEye, const char* tag) {
            Haruka::World::Water::OceanState sw = Haruka::World::Water::oceanDefaultState();
            sw.seaLevelM = 2000.0f;
            BufferHandle ubo = makeOceanStateUBO(sw, anchorRelEye, 5.0f);
            TextureHandle floorTex = makeFloor(-500.0f);
            Haruka::World::Terrain::TerrainNodeRenderer::Water wcfg;
            wcfg.heightTex = floorTex; wcfg.oceanParams = ubo; wcfg.on = true;
            r.setWater(wcfg);
            const glm::dvec3 cam = pc + up0 * (R + 5000.0);
            const glm::dvec3 fwd = -up0;
            const glm::dvec3 vup = glm::normalize(glm::cross(fwd, glm::dvec3(0, 1, 0)));
            const float aspect = (float)w / (float)h;
            const float fCot   = 1.0f / std::tan((float)fovY * 0.5f);
            glm::mat4 proj(0.0f);
            proj[0][0] = fCot / aspect; proj[1][1] = fCot; proj[2][3] = -1.0f; proj[3][2] = 1.0f;
            const glm::mat4 mvp = proj * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup))));
            std::vector<uint8_t> px((size_t)w * h * 4, 0xAA);
            for (int f = 0; f < 12; ++f) {
                Context* ctx = g_dev->beginFrame();
                if (!ctx) break;
                r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
                ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
                ctx->beginRenderPass({}, cv);
                r.draw(ctx, cam, pc, R, mvp);
                ctx->endRenderPass();
                if (!isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
                g_dev->endFrame();
                pumpWindowEvents();
                if (isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            }
            // HARUKA_WATER_PNG=<dir>: volcar la vista para MIRARLA. Los anillos concentricos del mar
            // (zancadas del terreno) no salen en ninguna cifra agregada; hay que verlos.
            if (const char* dirW = std::getenv("HARUKA_WATER_PNG")) {
                const bool flipW = detectRowFlip(px, w, h);
                std::vector<uint8_t> img(px.size());
                for (int y = 0; y < h; ++y) {
                    const int src = flipW ? (h - 1 - y) : y;
                    std::memcpy(&img[(size_t)y * w * 4], &px[(size_t)src * w * 4], (size_t)w * 4);
                }
                const std::string path = std::string(dirW) + "/" + (isVk ? "vk_" : "gl_")
                                       + "mar_nodos_" + std::string(tag).substr(0, 12) + ".png";
                Haruka::IO::writePNG(path, w, h, 4, img.data());
                std::printf("      -> %s\n", path.c_str());
            }
            // Luminancia de los pixeles de agua: media y desviacion. Un mar plano = una sola sombra.
            double s1 = 0.0, s2 = 0.0; size_t n = 0;
            for (size_t k = 0; k < px.size(); k += 4) {
                if (!(px[k+2] > px[k] + 4 && px[k+2] > 8)) continue;
                const double l = 0.3 * px[k] + 0.6 * px[k+1] + 0.1 * px[k+2];
                s1 += l; s2 += l * l; ++n;
            }
            const double mean = n ? s1 / n : 0.0, sd = n ? std::sqrt(std::max(s2 / n - mean * mean, 0.0)) : 0.0;
            std::printf("    sombra del mar (%s): %zu px de agua · luminancia media %.1f · desviacion %.2f\n", tag, n, mean, sd);
            g_dev->destroy(floorTex); g_dev->destroy(ubo);
            return sd;
        };
        const glm::dvec3 camA = pc + up0 * (R + 5000.0);
        const double sdOK  = shadeSigma(glm::vec3((pc + up0 * R) - camA), "ancla bajo la camara");
        const double sdBug = shadeSigma(glm::vec3(pc - camA),             "ancla en el centro del planeta = el bug");
        // Medido: 1,67 con el ancla bien y 0,34 con el bug (luminancia media ~11 desde 5 km).
        CHECK(sdOK > 1.0, "en el pase real la sombra del mar VARIA por la imagen (la ola viaja)");
        CHECK(sdOK > 3.0 * sdBug, "CONTRAPRUEBA: con la fase planetocentrica el mar era una sola sombra");
    }

    // ── ⚠️ ANILLOS DE ZANCADA EN EL MAR (19-09) ────────────────────────────────────────────────
    //
    // Andoni: "un bug del terreno que hace circulos; la vista de depuracion en azul claro y oscuro lo
    // muestra". Esa vista colorea por ZANCADA, y las zancadas son bandas CONCENTRICAS alrededor de la
    // camara. La pregunta que importa no es si la vista las ensena —las ensena por definicion— sino
    // si se notan SIN ella. Y hay un mecanismo para que se noten: la ola apaga cada tren cuando su
    // longitud baja de dos quads (`harukaShortWaveFade(k, quadM)`) y `quadM = texel x ZANCADA`, que
    // salta al DOBLE en cada banda. Al cruzar una, media docena de trenes se apagan de golpe: el mar
    // cambia de rugosidad en un circulo.
    //
    // Se mide la RUGOSIDAD (desviacion de luminancia en 3x3) por banda de distancia, desde 200 m
    // mirando 35 grados abajo, y se busca el mayor SALTO entre bandas contiguas.
    {
        Haruka::World::Water::OceanState stR = Haruka::World::Water::oceanDefaultState();
        stR.seaLevelM = 0.0f;
        BufferHandle oceanR = makeOceanStateUBO(stR);
        // ⚠️ FONDO DE COSTA (20 m), no mar abierto. Con 2 500 m la absorcion satura y el mar sale
        // del mismo azul en todo el cuadro: cualquier salto de zancada es invisible. Andoni: "sobre
        // todo en COSTAS". El relieve procedural sobre ese fondo da la variacion de profundidad.
        TextureHandle floorR = makeFloor(-20.0f);
        Haruka::World::Terrain::TerrainNodeRenderer::Water wc; wc.heightTex = floorR; wc.oceanParams = oceanR; wc.on = true;
        r.setWater(wc);
        const glm::dvec3 cam = pc + up0 * (R + 200.0);
        const glm::dvec3 tan0 = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
        const glm::dvec3 fwd = glm::normalize(tan0 * std::cos(glm::radians(35.0)) - up0 * std::sin(glm::radians(35.0)));
        const glm::dvec3 vup = glm::normalize(glm::cross(fwd, tan0));
        const float aspect = (float)w / (float)h;
        const float fCot   = 1.0f / std::tan((float)fovY * 0.5f);
        glm::mat4 projR(0.0f);
        projR[0][0] = fCot / aspect; projR[1][1] = fCot; projR[2][3] = -1.0f; projR[3][2] = 1.0f;
        const glm::mat4 mvpR = projR * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(vup))));
        std::vector<uint8_t> px((size_t)w * h * 4, 0xAA);
        for (int f = 0; f < 12; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = cv.color[1] = cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, mvpR);
            ctx->endRenderPass();
            if (!isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame(); pumpWindowEvents();
            if (isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
        if (const char* dirW = std::getenv("HARUKA_WATER_PNG")) {
            const bool flipW = detectRowFlip(px, w, h);
            std::vector<uint8_t> img(px.size());
            for (int y = 0; y < h; ++y) {
                const int src = flipW ? (h - 1 - y) : y;
                std::memcpy(&img[(size_t)y * w * 4], &px[(size_t)src * w * 4], (size_t)w * 4);
            }
            Haruka::IO::writePNG(std::string(dirW) + "/" + (isVk ? "vk_" : "gl_") + "mar_anillos.png", w, h, 4, img.data());
        }
        // Rugosidad por FILA (la fila es, a esta inclinacion, una banda de distancia).
        std::vector<double> rough(h, -1.0);
        for (int y = 1; y < h - 1; ++y) {
            double acc = 0.0; size_t n = 0;
            for (int x = 1; x < w - 1; ++x) {
                const size_t k = ((size_t)y * w + x) * 4;
                if (!(px[k+2] > px[k] + 4 && px[k+2] > 8)) continue;   // solo agua
                double m = 0.0; int c = 0;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    const size_t q = ((size_t)(y + dy) * w + (x + dx)) * 4;
                    m += (px[q] + px[q+1] + px[q+2]) / 3.0; ++c;
                }
                m /= c;
                const double l = (px[k] + px[k+1] + px[k+2]) / 3.0;
                acc += std::abs(l - m); ++n;
            }
            if (n > (size_t)w / 8) rough[y] = acc / (double)n;
        }
        double jump = 0.0; int jumpRow = -1; double rMin = 1e9, rMax = -1.0;
        for (int y = 2; y < h - 2; ++y) {
            if (rough[y] < 0.0 || rough[y-1] < 0.0) continue;
            rMin = std::min(rMin, rough[y]); rMax = std::max(rMax, rough[y]);
            const double j = std::abs(rough[y] - rough[y-1]);
            if (j > jump) { jump = j; jumpRow = y; }
        }
        std::printf("    ANILLOS: rugosidad del mar por fila (desde 200 m, -35 grados): min %.2f · max %.2f"
                    " · mayor salto entre filas vecinas %.2f (fila %d)\n", rMin, rMax, jump, jumpRow);
        // ── ⚠️ LA ZANCADA BAJO EL AGUA SOMERA, Y LO QUE CUESTA ─────────────────────────────────
        // El salto de la banda de zancada (hasta 3,75 m de fondo) es invisible en mar abierto y se
        // ve como un CIRCULO en la costa, porque el regimen somero es no lineal en la profundidad.
        // El tope (`kWaterReliefFrac`) afina la malla donde el agua es poco profunda: eso ACERCA lo
        // dibujado al campo (menos disparidad, no mas) y cuesta triangulos. Se miden los dos.
        {
            // ⚠️ MIRANDO CASI AL HORIZONTE (−3 grados), no a −35: a 35 grados abajo todo cae a pocos
            // km y la zancada ya es la fina, asi que el tope no puede morder y el test mediria 0 por
            // construccion (paso una vez). Las bandas gruesas viven a decenas de km.
            const glm::dvec3 fwdH = glm::normalize(tan0 * std::cos(glm::radians(3.0)) - up0 * std::sin(glm::radians(3.0)));
            const glm::dvec3 vupH = glm::normalize(glm::cross(fwdH, tan0));
            const glm::mat4 mvpH = projR * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwdH), glm::vec3(vupH))));
            // ⚠️ Y CON MUESTREADOR DE ALTURA: sin el, el pool no sabe el rango de cada nodo
            // (`rangeOf` invalido) y el tope —que se apoya en la cota MINIMA del nodo— no puede
            // morder. Medido: 0 nodos acotados hasta que se puso esto.
            // ⚠️ Y SE VUELVE A PONER EN CADA TIRADA: el pool es una cache y los residentes guardan
            // el rango del muestreador con el que se generaron; `setHeightSampler` los re-acota
            // (`TerrainNodePool::rerange`). Sin eso las dos tiradas daban LO MISMO (45 acotados,
            // 14 particiones) porque el fondo de -2 000 m nunca llegaba al rango.
            static float s_fondo = -3.0f;
            auto tirada = [&](float fondo, size_t& capped, size_t& tris, uint32_t& strideMax,
                              size_t& splits, size_t& selected) {
                s_fondo = fondo;
                r.setHeightSampler([](const glm::dvec3&, void*) { return s_fondo; }, nullptr);
                TextureHandle fl = makeFloor(fondo);
                Haruka::World::Terrain::TerrainNodeRenderer::Water wcap; wcap.heightTex = fl; wcap.oceanParams = oceanR; wcap.on = true;
                r.setWater(wcap);
                Haruka::World::Terrain::TerrainNodeRenderer::FrameStats last{};
                for (int f = 0; f < 8; ++f) {
                    Context* ctx = g_dev->beginFrame();
                    if (!ctx) break;
                    r.prepare(ctx, cam, pc, R, fwdH, radPerPx, cone);
                    ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
                    ctx->beginRenderPass({}, cv);
                    last = r.draw(ctx, cam, pc, R, mvpH);
                    ctx->endRenderPass();
                    g_dev->endFrame(); pumpWindowEvents();
                }
                capped = last.waterStrideCapped; tris = last.tris; strideMax = last.stride;
                splits = last.waterSplit; selected = last.selected;
                g_dev->destroy(fl);
            };
            size_t cS = 0, cD = 0, tS = 0, tD = 0, pS = 0, pD = 0, nS = 0, nD = 0; uint32_t sS = 0, sD = 0;
            tirada(-3.0f,    cS, tS, sS, pS, nS);     // costa: 3 m de fondo
            tirada(-2000.0f, cD, tD, sD, pD, nD);     // mar abierto
            std::printf("    ZANCADA bajo el agua: costa (3 m) %zu nodos acotados · %.2f M tris · zancada max %u"
                        "  |  mar abierto (2 000 m) %zu acotados · %.2f M tris · zancada max %u\n",
                        cS, tS / 1e6, sS, cD, tD / 1e6, sD);
            // Y la PARTICION extra por nivel (`nodeShallowWantsSplit`), sobre el rango REAL que
            // publica el pool: es la version por nivel del tope de zancada, y quita los anillos de
            // la frontera entre niveles hasta 2x la distancia de particion. Coste en nodos.
            std::printf("    NIVEL bajo el agua: costa (3 m) %zu particiones del agua, %zu nodos seleccionados"
                        "  |  mar abierto %zu particiones, %zu nodos\n", pS, nS, pD, nD);
            CHECK(cS > 0, "en la costa la zancada se acota (si no, el tope no hace nada)");
            CHECK(cD == 0, "CONTRAPRUEBA: en mar abierto NO se acota ni un nodo (el tope es de 200 m alli)");
            CHECK(tS >= tD, "y el coste va donde el agua es somera: mas triangulos en la costa que en mar abierto");
            CHECK(pS > 0 && nS > nD, "en la costa el agua pide particiones que la pantalla no pedia, y cuesta nodos");
            CHECK(pD == 0, "CONTRAPRUEBA: en mar abierto (presupuesto 500 m) el agua no pide ni una particion");

            // ── ¿EL AGUA CUESTA POR VERTICE O POR PIXEL? (Andoni, 20-09: "sigue con el .5") ──────
            // En el juego `v5.agua` son 10-16 ms en la costa, el mayor pase del frame, y apagar los
            // parches de costa no lo baja. Dos sospechosos: el vertex (4 evaluaciones de
            // `harukaTerrainDetail` por vertice desde el corte continuo) o el fragment (Green,
            // espuma, absorcion). Se separan con el MISMO dibujo a dos tamaños de viewport: los
            // vertices son los mismos y los pixeles 1/4. Con c = V + F y c/4 = V + F/4:
            //     V = (4·c_cuarto − c_entero) / 3,   F = c_entero − V.
            // Escena: la costa de arriba (fondo 3 m, 200 m de altura, −3 grados), agua encendida.
            {
                s_fondo = -3.0f;
                r.setHeightSampler([](const glm::dvec3&, void*) { return s_fondo; }, nullptr);
                TextureHandle fl = makeFloor(-3.0f);
                std::vector<uint8_t> one(4, 0);
                auto timeCoast = [&](bool waterOn, int div, int frames) {
                    Haruka::World::Terrain::TerrainNodeRenderer::Water wc; wc.heightTex = fl; wc.oceanParams = oceanR; wc.on = waterOn;
                    r.setWater(wc);
                    auto frame = [&]() {
                        if (Context* c = g_dev->beginFrame()) {
                            r.prepare(c, cam, pc, R, fwdH, radPerPx, cone);
                            ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
                            c->beginRenderPass({}, cv);
                            c->setViewport(0, 0, w / div, h / div);
                            r.draw(c, cam, pc, R, mvpH);
                            // ⚠️ Y SE DEJA COMO ESTABA: en GL el viewport persiste entre pases y el
                            // test siguiente ("LINEA DEL MAR") leia un cuadro a 128x128 con el resto
                            // negro (100 % negro, 0 % agua). En Vulkan no se notaba.
                            c->setViewport(0, 0, w, h);
                            c->endRenderPass();
                            g_dev->endFrame(); pumpWindowEvents();
                        }
                    };
                    for (int f = 0; f < 8; ++f) frame();
                    g_dev->readPixels(0, 0, 1, 1, Format::RGBA8, one.data());   // sync
                    const auto t0 = std::chrono::high_resolution_clock::now();
                    for (int f = 0; f < frames; ++f) frame();
                    g_dev->readPixels(0, 0, 1, 1, Format::RGBA8, one.data());
                    return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count() / frames;
                };
                const double tOff1 = timeCoast(false, 1, 30), tOff2 = timeCoast(false, 2, 30);
                const double tOn1  = timeCoast(true,  1, 30), tOn2  = timeCoast(true,  2, 30);
                const double agua1 = tOn1 - tOff1, agua2 = tOn2 - tOff2;
                const double aguaV = std::max((4.0 * agua2 - agua1) / 3.0, 0.0), aguaF = agua1 - aguaV;
                const double terrV = std::max((4.0 * tOff2 - tOff1) / 3.0, 0.0), terrF = tOff1 - terrV;
                const bool vs = (tOff1 > 15.5 && tOff1 < 18.0);
                std::printf("    AGUA, VERTICE O PIXEL (costa 3 m, %dx%d contra %dx%d): terreno %.3f / %.3f ms · agua %.3f / %.3f ms\n"
                            "      -> agua: vertice %.3f ms (%.0f %%) · pixel %.3f ms  |  terreno: vertice %.3f ms (%.0f %%) · pixel %.3f ms%s\n",
                            w, h, w / 2, h / 2, tOff1, tOff2, agua1, agua2,
                            aguaV, agua1 > 0 ? 100.0 * aguaV / agua1 : 0.0, aguaF,
                            terrV, tOff1 > 0 ? 100.0 * terrV / tOff1 : 0.0, terrF,
                            vs ? "  (VSYNC: no vale)" : "");
                {
                    // DONDE ESTAN LOS VERTICES: nodos de agua por zancada y triangulos. Un nodo cuesta
                    // (129/zancada)^2 vertices este donde este, asi que la cuenta por zancada dice
                    // que parte del pase es la malla fina de cerca y que parte el numero de nodos.
                    const auto& st = r.stats();
                    std::printf("      nodos de agua %zu (%zu con mas zancada que el terreno) · %.2f M tris de agua, %.2f M de terreno\n"
                                "      por zancada (1,2,4,8,16,32,64): agua", st.waterNodes, st.waterCoarsened,
                                st.waterTris / 1e6, st.tris / 1e6);
                    for (int k = 0; k < 7; ++k) std::printf(" %zu", st.waterByStride[k]);
                    std::printf("  ·  tris terreno (M)");
                    for (int k = 0; k < 7; ++k) std::printf(" %.2f", st.trisByStride[k] / 1e6);
                    std::printf("\n");
                }
                CHECK(vs || agua1 > 0.0, "el coste del agua en la costa queda medido");
                baselineMetric("agua.costa_ms", agua1, 0.30, true, true, !vs);
                baselineMetric("agua.costa_vertice_ms", aguaV, 0.30, true, true, !vs);
                r.setWater(Haruka::World::Terrain::TerrainNodeRenderer::Water{});
                g_dev->destroy(fl);
            }
        }
        CHECK(rMax > 0.0, "hay mar con rugosidad que medir");
        CHECK(jump < 0.25 * std::max(rMax, 1e-6), "el mar NO cambia de rugosidad en un escalon (anillo de zancada): el mayor salto entre filas es menos del 25 % del maximo");
        g_dev->destroy(floorR); g_dev->destroy(oceanR);
    }

    // ── ⚠️ BLOQUES NEGROS EN LA LINEA DEL MAR (captura de Andoni, 18-09) ────────────────────────
    // Desde 2 m mirando al horizonte con MAR en todo el suelo, en la banda justo bajo el horizonte
    // (0,05-1,5 grados) no puede haber pixeles NEGROS: o es agua (azul) o es cielo/nada. Se cuentan
    // los negros por FILA para ver si van a bloques (nodos) y se imprime.
    {
        Haruka::World::Water::OceanState stH = Haruka::World::Water::oceanDefaultState();
        stH.seaLevelM = 0.0f;
        BufferHandle oceanH = makeOceanStateUBO(stH);
        TextureHandle floorTex = makeFloor(-500.0f);
        Haruka::World::Terrain::TerrainNodeRenderer::Water wcfg;
        wcfg.heightTex = floorTex; wcfg.oceanParams = oceanH; wcfg.on = true;
        r.setWater(wcfg);
        const glm::dvec3 cam = pc + up0 * (R + 2.0);
        const glm::dvec3 fwd = glm::normalize(glm::cross(up0, glm::dvec3(0, 1, 0)));
        const float aspect = (float)w / (float)h;
        const float fCot   = 1.0f / std::tan((float)fovY * 0.5f);
        glm::mat4 proj(0.0f);
        proj[0][0] = fCot / aspect; proj[1][1] = fCot; proj[2][3] = -1.0f; proj[3][2] = 1.0f;
        const glm::mat4 mvp = proj * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f), glm::vec3(fwd), glm::vec3(up0))));
        std::vector<uint8_t> px((size_t)w * h * 4, 0xAA);
        for (int f = 0; f < 12; ++f) {
            Context* ctx = g_dev->beginFrame();
            if (!ctx) break;
            r.prepare(ctx, cam, pc, R, fwd, radPerPx, cone);
            ClearValues cv; cv.clearColor = true; cv.clearDepth = true;
            cv.color[0] = 0.0f; cv.color[1] = 0.0f; cv.color[2] = 0.0f; cv.color[3] = 1.0f; cv.depth = 0.0f;
            ctx->beginRenderPass({}, cv);
            r.draw(ctx, cam, pc, R, mvp);
            ctx->endRenderPass();
            if (!isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
            g_dev->endFrame(); pumpWindowEvents();
            if (isVk && f == 11) g_dev->readPixels(0, 0, w, h, Format::RGBA8, px.data());
        }
        // Filas: el horizonte geometrico esta a −0,045 grados desde 2 m; la mitad "agua" del cuadro es
        // la que tiene agua (azul). Se busca la orientacion por donde hay azul.
        const double tanHalf = std::tan(fovY * 0.5);
        size_t blueTop = 0, blueBot = 0;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const size_t k = ((size_t)y * w + x) * 4;
            if (px[k+2] > px[k] + 4 && px[k+2] > 8) { if (y < h / 2) ++blueTop; else ++blueBot; }
        }
        const bool waterIsTop = blueTop > blueBot;
        size_t nBand = 0, nBlack = 0, nBlue = 0; int rowsWithBlack = 0;
        for (int y = 0; y < h; ++y) {
            double e = std::atan(((h * 0.5 - (y + 0.5)) / (h * 0.5)) * tanHalf) * 180.0 / 3.14159265358979;
            if (waterIsTop) e = -e;                       // e < 0 = bajo el horizonte
            if (e > -0.05 || e < -1.5) continue;
            size_t rowBlack = 0;
            for (int x = 0; x < w; ++x) {
                const size_t k = ((size_t)y * w + x) * 4;
                if (px[k] == 0xAA && px[k+1] == 0xAA && px[k+2] == 0xAA) continue;
                ++nBand;
                if (px[k] < 6 && px[k+1] < 6 && px[k+2] < 6) { ++nBlack; ++rowBlack; }
                else if (px[k+2] > px[k] + 4 && px[k+2] > 8) ++nBlue;
            }
            if (rowBlack > 0) ++rowsWithBlack;
        }
        std::printf("    LINEA DEL MAR desde 2 m (banda 0,05-1,5 grados bajo el horizonte): %zu px · agua %.1f %% · NEGRO %.1f %% (en %d filas)\n",
                    nBand, nBand ? 100.0 * nBlue / nBand : 0.0, nBand ? 100.0 * nBlack / nBand : 0.0, rowsWithBlack);
        CHECK(nBand > 0 && nBlack < nBand / 50, "bajo el horizonte, sobre el mar, no hay pixeles negros (< 2 %)");
        g_dev->destroy(floorTex); g_dev->destroy(oceanH);
    }

    // ⚠️ EL CENTINELA PRIMERO: un `readPixels` que no escribe deja 0xAA y el conteo daria 0, o sea
    // que el test acusaria al pase de no dibujar cuando el que fallo fue el instrumento. Ya paso una
    // vez en el camino de Vulkan.
    // ── ⚠️ EL COSTE DEL PASE DE AGUA, QUE NUNCA SE HABIA MEDIDO ────────────────────────────────
    //
    // Estaba en la lista de deudas: *"el perfil de rendimiento del agua no se ha corrido nunca"*, y
    // encima se acaba de DOBLAR el trabajo del vertice (4 -> 8 trenes de Gerstner). Doblar un coste
    // desconocido es exactamente lo que paso con el geomorph, asi que aqui se mide.
    //
    // El metodo: el MISMO pase con `Water::on` en false y en true, 40 frames cada uno, y un
    // `readPixels` al final de cada tanda que FUERZA el sync — sin el se estaria midiendo lo que
    // tarda la CPU en encolar, no lo que tarda la GPU en dibujar. La diferencia es el agua.
    {
        TextureHandle floorTex = makeFloor(-500.0f);
        std::vector<uint8_t> one(4, 0);
        const glm::dvec3 camT = up0 * (R + 3000.0);
        const glm::dvec3 fwdT = -up0;
        const glm::dvec3 vupT = glm::normalize(glm::cross(fwdT, glm::dvec3(0, 1, 0)));
        const float aspectT = (float)w / (float)h;
        const float fCotT   = 1.0f / std::tan((float)fovY * 0.5f);
        glm::mat4 projT(0.0f);
        projT[0][0] = fCotT / aspectT; projT[1][1] = fCotT;
        projT[2][3] = -1.0f;           projT[3][2] = 1.0f;
        const glm::mat4 mvpT = projT * glm::mat4(glm::mat3(glm::lookAt(glm::vec3(0.0f),
                                                    glm::vec3(fwdT), glm::vec3(vupT))));
        auto timeFrames = [&](bool waterOn, int frames) {
            Haruka::World::Terrain::TerrainNodeRenderer::Water wc;
            wc.heightTex = floorTex; wc.oceanParams = oceanUBO; wc.on = waterOn;
            r.setWater(wc);
            // Calentamiento: la primera pasada crea/compila y mediria eso.
            for (int f = 0; f < 8; ++f) {
                if (Context* c = g_dev->beginFrame()) {
                    r.prepare(c, camT, pc, R, fwdT, radPerPx, cone);
                    ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
                    c->beginRenderPass({}, cv); r.draw(c, camT, pc, R, mvpT); c->endRenderPass();
                    g_dev->endFrame(); pumpWindowEvents();
                }
            }
            g_dev->readPixels(0, 0, 1, 1, Format::RGBA8, one.data());   // sync
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (int f = 0; f < frames; ++f) {
                if (Context* c = g_dev->beginFrame()) {
                    r.prepare(c, camT, pc, R, fwdT, radPerPx, cone);
                    ClearValues cv; cv.clearColor = true; cv.clearDepth = true; cv.depth = 0.0f;
                    c->beginRenderPass({}, cv); r.draw(c, camT, pc, R, mvpT); c->endRenderPass();
                    g_dev->endFrame(); pumpWindowEvents();
                }
            }
            g_dev->readPixels(0, 0, 1, 1, Format::RGBA8, one.data());   // sync: cierra la cuenta
            return std::chrono::duration<double, std::milli>(
                       std::chrono::high_resolution_clock::now() - t0).count() / (double)frames;
        };
        const double msOff = timeFrames(false, 40);
        const double msOn  = timeFrames(true,  40);
        std::printf("    COSTE DEL PASE: terreno solo %.3f ms/frame · terreno+agua %.3f ms/frame"
                    "  ->  el agua cuesta %.3f ms (%.1f %% de un frame de 16,7)\n",
                    msOff, msOn, msOn - msOff, 100.0 * (msOn - msOff) / 16.7);
        std::printf("    (con %d trenes de Gerstner por vertice de agua)\n", Haruka::World::Water::OCEAN_WAVES);
        // ⚠️ Y SI HAY VSYNC, ESTA CIFRA NO VALE. Con FIFO cualquier ms/frame se clava en 16,67 y la
        // diferencia mide el hueco que sobra, no el pase. Es la trampa que ya esta escrita en el
        // TODO ("para medir tiempos, HARUKA_NO_VSYNC=1") y sin este aviso la segunda pasada del banco
        // publicaria 0,1 ms como si fuera el coste del agua.
        const bool vsync = (msOff > 15.5 && msOff < 18.0);
        if (vsync)
            std::printf("    \033[33m⚠️ la base son %.1f ms = VSYNC (FIFO): esta medida NO es el coste del "
                        "pase. Repite con HARUKA_NO_VSYNC=1.\033[0m\n", msOff);
        CHECK(msOff > 0.0 && msOn > 0.0, "el coste del pase de agua queda MEDIDO (deuda cerrada)");
        // No se fija un techo: la cifra depende de la GPU del que corra el banco. Lo que se fija es
        // que el agua no multiplique el pase, que es la forma en que esto se volveria un problema.
        CHECK(vsync || msOn < msOff * 6.0 + 1.0,
              "el agua no multiplica por 6 el coste del pase de terreno (salvo con vsync, que no mide)");
        baselineMetric("agua.pase_ms", msOn - msOff, 0.30, true, true, !vsync);
        baselineMetric("terreno.pase_ms", msOff, 0.30, true, true, !vsync);
        r.setWater(Haruka::World::Terrain::TerrainNodeRenderer::Water{});
        g_dev->destroy(floorTex);
    }

    CHECK(sSea < total / 2, "el readback escribio (si no, el conteo no significa nada)");
    CHECK(blueSea > total / 20, "el pase de nodos DIBUJA agua sobre un fondo de mar");
    // CONTRAPRUEBA: sobre tierra el mismo pase no debe pintar ni una gota. Sin esto, "hay pixeles
    // azules" no distingue el agua de cualquier cosa que el pase pinte.
    CHECK(blueLand < blueSea / 10,
          "CONTRAPRUEBA: sobre TIERRA el agua se descarta — es la profundidad quien manda");
    r.shutdown();
    if (valid(oceanUBO)) g_dev->destroy(oceanUBO);
}

// ⚠️ SIN USO desde el commit 5025b4f (nadie la llama). Se conserva al partir RHITestMain.cpp por si un
// test de agua la recupera; si no, se puede borrar.
// ================================================================================================
// EL MAR DIBUJA PÍXELES, Y SOLO DONDE HAY AGUA.
//
// ⚠️ POR QUÉ NO BASTA CON "¿ENLAZA EL PIPELINE?". Se escribió antes un test que solo comprobaba que
// `createPipeline` devolviera un handle válido, se le hizo la CONTRAPRUEBA —romper `ocean_far.vert`
// quitándole el varying `vLoc`, que es el bug real que tumbó la esfera del océano— y el test SIGUIÓ
// PASANDO. El enlazado de módulos SPIR-V tolera una entrada que nadie escribe; el error solo salta
// por el camino GLSL. Un test que no falla cuando el bug está es una tautología: peor que nada,
// porque da confianza falsa.
//
// Esto mira PÍXELES, que es lo único que el usuario ve. Y no comprueba "hay algo": comprueba que el
// shader DISCRIMINA. Con el mismo pipeline y la misma geometría, cambiando UN dato de entrada:
//
//   fondo a -1000 m (océano) → tiene que ESCRIBIR   (hay agua)
//   fondo a +1000 m (tierra) → tiene que DESCARTAR  (no hay agua)
//
// Si las dos salen iguales, algo está roto aunque todo "funcione": el discard, la profundidad, el
// muestreo del bake o la propia superficie. Y de paso caza NaN y saturaciones, que son las dos
// formas en que este motor se ha roto hoy en Vulkan.
// ================================================================================================
[[maybe_unused]] static bool oceanDrawsPixels(float bakeHeightM, uint8_t out[4])
{
    // Bake de altura sintético: una textura 1x1 con la cota que queramos. `harukaSampleHeightField`
    // la lee con texelFetch, así que un solo téxel basta para decidir mar o tierra.
    TextureDesc td;
    td.width = td.height = 1;
    td.format = Format::R32F;
    td.filter = Filter::Nearest;
    td.wrap   = Wrap::ClampToEdge;
    td.initialData = &bakeHeightM;
    TextureHandle height = g_dev->createTexture(td);
    if (!valid(height)) return false;

    // SimplePlanetUBO. uMVP = identidad y uCenter = 0 para que los vértices caigan tal cual en clip
    // space: así el triángulo cubre la pantalla sin depender de una cámara real.
    struct PlanetUBO {
        float mvp[16];
        float center[4], lightDir[4], lightColor[4], ambient[4], extra[4], debug[4], texAnchor[4];
    } u{};
    u.mvp[0] = u.mvp[5] = u.mvp[10] = u.mvp[15] = 1.0f;
    u.lightDir[1]   = 1.0f;                                  // sol al cénit
    u.lightColor[0] = u.lightColor[1] = u.lightColor[2] = 1.0f;
    u.ambient[0] = 0.2f; u.ambient[1] = 0.2f; u.ambient[2] = 0.3f;
    u.extra[3] = 6371000.0f;                                 // radio del planeta
    u.debug[2] = 1.0f;                                       // hay bake de altura
    BufferHandle ubo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(u), &u, BufferMemory::Dynamic);

    // Campo de agua interior: presente pero VACÍO (misc.w = 0). Se ata SIEMPRE porque en Vulkan un
    // bloque sin atar es indefinido — la misma regla que el motor aprendió con el UBO de marea.
    struct InlandUBO { float anchor[4], tanU[4], tanV[4], misc[4]; } iu{};
    BufferHandle inlandUbo = g_dev->createBuffer(BufferUsage::Uniform, sizeof(iu), &iu, BufferMemory::Dynamic);
    float zero = 0.0f;
    BufferHandle inlandSsbo = g_dev->createBuffer(BufferUsage::Storage, sizeof(zero), &zero, BufferMemory::Dynamic);

    // Un triángulo que cubre la pantalla, en el formato que espera `ocean_far.vert` (pos3 + normal3).
    const float verts[] = {
        -1.0f, -1.0f, 1.0f,   0.0f, 0.0f, 1.0f,
         3.0f, -1.0f, 1.0f,   0.0f, 0.0f, 1.0f,
        -1.0f,  3.0f, 1.0f,   0.0f, 0.0f, 1.0f,
    };
    BufferHandle vb = g_dev->createBuffer(BufferUsage::Vertex, sizeof(verts), verts, BufferMemory::Static);

    const std::string sh = Haruka::Renderer::Shader::baseDir() + "shaders/";
    const std::string vs = sh + "planet/ocean_far.vert", fs = sh + "planet/ocean.frag";
    PipelineDesc pd;
    pd.vertexPath   = vs.c_str();
    pd.fragmentPath = fs.c_str();
    pd.vertexLayout.strides    = { (uint32_t)(6 * sizeof(float)) };
    pd.vertexLayout.attributes = { { 0, 0, Format::RGB32F },
                                   { 1, (uint32_t)(3 * sizeof(float)), Format::RGB32F } };
    pd.topology     = PrimitiveTopology::Triangles;
    pd.depth.test   = false; pd.depth.write = false;
    pd.cull         = CullMode::None;
    pd.blend.enable = false;                 // sin mezcla: lo que se lee es lo que escribió el shader
    PipelineHandle pipe = g_dev->createPipeline(pd);
    if (!valid(pipe)) {
        g_dev->destroy(height); g_dev->destroy(ubo); g_dev->destroy(vb);
        g_dev->destroy(inlandUbo); g_dev->destroy(inlandSsbo);
        return false;
    }

    const bool isVk = (g_dev->backend() == Backend::Vulkan);
    Context* c = g_dev->beginFrame();
    if (c) {
        ClearValues cv;
        cv.clearColor = true;
        cv.color[0] = 1.0f; cv.color[1] = 0.0f; cv.color[2] = 0.0f; cv.color[3] = 1.0f;  // ROJO
        cv.clearDepth = true; cv.depth = 0.0f;
        c->beginRenderPass({}, cv);
        c->bindPipeline(pipe);
        c->bindUniformBuffer(0, ubo);
        c->bindUniformBuffer(24, inlandUbo);
        c->bindStorageBuffer(25, inlandSsbo);
        c->bindTexture(16, height);
        c->bindVertexBuffer(vb);
        c->draw(3);
        c->endRenderPass();
    }
    std::memset(out, 0, 4);
    if (!isVk) g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);
    g_dev->endFrame();
        pumpWindowEvents();
    if (isVk)  g_dev->readPixels(128, 128, 1, 1, Format::RGBA8, out);

    g_dev->destroy(pipe); g_dev->destroy(height); g_dev->destroy(ubo);
    g_dev->destroy(vb); g_dev->destroy(inlandUbo); g_dev->destroy(inlandSsbo);
    return true;
}
