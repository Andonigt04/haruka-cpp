#define GLM_ENABLE_EXPERIMENTAL
#include <cstdio>
#include <cstdlib>
#include "character.h"
#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <iostream>
#include <algorithm>
#include <glm/gtx/quaternion.hpp>

namespace Haruka {

namespace {
    glm::dvec3 safeNormalize(const glm::dvec3& v, const glm::dvec3& fallback) {
        double len = glm::length(v);
        if (len < 1e-9) return fallback;
        return v / len;
    }

    void buildSurfaceBasis(
        const glm::dvec3& gravityUp,
        const glm::quat& cameraOrientation,
        glm::dvec3& outUp,
        glm::dvec3& outForward,
        glm::dvec3& outRight
    ) {
        outUp = safeNormalize(gravityUp, glm::dvec3(0.0, 1.0, 0.0));

        glm::vec3 camForward3 = cameraOrientation * glm::vec3(0, 0, -1);
        glm::dvec3 camForward = glm::dvec3(camForward3);

        // Proyectar la cámara sobre el plano tangente del planeta
        outForward = camForward - outUp * glm::dot(camForward, outUp);
        outForward = safeNormalize(outForward, glm::cross(glm::dvec3(0.0, 0.0, 1.0), outUp));

        outRight = glm::cross(outForward, outUp);
        outRight = safeNormalize(outRight, glm::dvec3(1.0, 0.0, 0.0));
    }
}

Character::Character(const glm::dvec3& position, const std::string& userId)
    : userId(userId), position(position), velocity(0.0) {
    
    localPlayer = !userId.empty();
    
    if (localPlayer) {
        camera = std::make_unique<Camera>(WorldPos(position.x, position.y + currentHeight * 0.9, position.z));
    }
    
    forward = glm::vec3(0, 0, -1);
    right = glm::vec3(1, 0, 0);
    
    double posLen = glm::length(position);
    upDirection = posLen > 1e-6 ? (position / posLen) : Haruka::WorldPos(0.0, 1.0, 0.0);
    
    // CORRECCIÓN: Convertir Euler (Pitch, Yaw, Roll) a Cuaternión
    Haruka::Rotation initialRot = glm::dquat(glm::dvec3(glm::radians((double)pitch), glm::radians((double)yaw), 0.0));
    
    lastSyncPos = Haruka::WorldPos(position.x, position.y + currentHeight * 0.9, position.z);
    lastSyncRot = initialRot;
    targetPosition = lastSyncPos;
    targetRotation = initialRot;
    
    std::cout << "[Character] Created" << (localPlayer ? " (local)" : " (remote)") << " at " 
              << position.x << ", " << position.y << ", " << position.z << std::endl;
}

Character::~Character() {}

void Character::update(float deltaTime) {
    if (physicsBody)
    {
        // physicsBody->position is the body centre; foot = centre - up*radius.
        // up es RADIAL (hacia fuera del planeta), no (0,1,0): si no, lejos del polo
        // norte el pie queda desplazado de lado → el jugador se "traba" al andar.
        glm::dvec3 up = glm::dvec3(getEffectiveUp());
        double ul = glm::length(up);
        up = (ul > 1e-6) ? up / ul : glm::dvec3(0.0, 1.0, 0.0);
        position = glm::dvec3(physicsBody->position) - up * (double)physicsBody->radius;
        velocity = glm::dvec3(physicsBody->velocity);

        // In flight mode gravity must not accumulate: zero the vertical velocity
        // so the physics engine can't drag the character downward.
        if (flightMode) {
            physicsBody->velocity.y = 0.0;
            velocity.y = 0.0;
        }
    }
    
    if (!localPlayer)
    {
        position = glm::mix(position, targetPosition, (double)(deltaTime * interpolationSpeed));
        
        Haruka::Rotation currentRot = glm::dquat(glm::dvec3(glm::radians((double)pitch), glm::radians((double)yaw), 0.0));
        Haruka::Rotation newRot = glm::slerp(currentRot, targetRotation, (double)(deltaTime * interpolationSpeed));
        
        glm::dvec3 euler = glm::eulerAngles(newRot);
        pitch = glm::degrees(euler.x);
        yaw = glm::degrees(euler.y);
    }
    
    checkGrounded();
    applyWind(deltaTime);
    updateState();
    
    if (localPlayer)
    {
        // Keep camera in sync even when processInput() is skipped (chat open,
        // no window focus). processInput() will call updateCamera() again after
        // applying input, producing a zero-lag result on frames where it runs.
        updateCamera();

        syncTimer += deltaTime;
        if (syncTimer >= syncInterval)
        {
            syncTimer = 0.0f;
        }
    }
    
    float targetHeight = crouched ? crouchingHeight : standingHeight;
    currentHeight += (targetHeight - currentHeight) * deltaTime * 10.0f;
}

// ── EL VIENTO EMPUJA ────────────────────────────────────────────────────────────────────────────
//
// Arrastre aerodinámico de verdad, no un empujón con una constante inventada:
//
//     a = ½·ρ·Cd·A/m · |v_aire − v_cuerpo|·(v_aire − v_cuerpo)
//
// Con ρ = 1,2 kg/m³, Cd ≈ 1,0 (una persona no es aerodinámica), A ≈ 0,7 m² y m = 80 kg sale el
// coeficiente de abajo. Que vaya con el CUADRADO de la velocidad es lo que hace que esto se sienta
// bien sin tocar nada: a 10 m/s son 0,5 m/s² (te despeina), a 30 m/s son 4,7 (cuesta andar) y a
// 90 m/s —el núcleo de un tornado— son 42 m/s², cuatro veces la gravedad. No hace falta un caso
// especial para "te levanta": sale de la fórmula.
void Character::applyWind(float deltaTime) {
    if (!m_windProvider || !physicsBody || flightMode || deltaTime <= 0.0f) return;

    const glm::dvec3 vAir = m_windProvider(position);
    glm::dvec3 up = glm::dvec3(getEffectiveUp());
    const double ul = glm::length(up);
    up = (ul > 1e-6) ? up / ul : glm::dvec3(0.0, 1.0, 0.0);

    constexpr double kDrag     = 0.00525;  // ½·ρ·Cd·A/m, ver arriba
    constexpr double kMaxAccel = 60.0;     // techo de cordura: ni el peor vórtice rompe la integración
    // ⚠️ ERA 8,0 (μ = 0,8, el rozamiento de una suela agarrando) Y ESO DEJA AL TEMPORAL SIN EFECTO:
    // el arrastre a 35 m/s son 0,00525·35² = **6,4 m/s²**, por debajo de 8, así que un viento de
    // 126 km/h no movía al jugador ni un centímetro. Medido en `wind_pushes_character`, que imprime
    // la deriva a cuatro vientos justo para que esto no se pueda decidir a ojo.
    //
    // El fallo era de modelo, no de número: a una persona el viento no la hace DESLIZAR, la hace
    // PERDER PIE — vuelca antes de que la suela patine. 4,5 m/s² es el umbral efectivo de eso: a
    // 25 m/s (90 km/h) aguantas de pie con esfuerzo y a 35 empiezas a ser arrastrado, que es lo que
    // pasa de verdad.
    constexpr double kFriction = 4.5;      // m/s²: no es la suela, es perder pie

    const glm::dvec3 rel = vAir - glm::dvec3(physicsBody->velocity);
    const double s = glm::length(rel);
    if (s < 1e-6) { /* sin viento relativo no hay fuerza, pero el rozamiento sigue actuando */ }
    glm::dvec3 accel = rel * (kDrag * s);
    const double al = glm::length(accel);
    if (al > kMaxAccel) accel *= kMaxAccel / al;

    // La componente RADIAL va directa a la velocidad del cuerpo: es la que compite con la gravedad y
    // la que te levanta. No pasa por la deriva porque `move()` ya conserva lo radial.
    const double aRad = glm::dot(accel, up);
    physicsBody->velocity += up * (aRad * (double)deltaTime);

    // La TANGENTE se acumula aparte (ver `m_windDrift`). Al cuerpo se le aplica el INCREMENTO, no el
    // total: así la velocidad de locomoción que escribió `move()` sigue ahí y no hay que reconstruir
    // "qué parte de la tangencial era andar y qué parte era viento" — que no se puede saber.
    const glm::dvec3 driftAntes = m_windDrift;
    m_windDrift += (accel - up * aRad) * (double)deltaTime;

    // ROZAMIENTO: de pie en el suelo, el viento tiene que GANARLE al rozamiento para moverte. Sin
    // esto una brisa de 10 m/s te iría desplazando sin parar, porque nada frena la deriva; y con un
    // frenado proporcional (·0,9 por frame) el resultado depende de los FPS. Un rozamiento de
    // Coulomb —deceleración constante hasta parar— es lo que hace que por debajo de cierto viento
    // no pase nada y por encima te arrastre, que es justo el comportamiento que se quiere.
    if (grounded) {
        const double d = glm::length(m_windDrift);
        const double dec = kFriction * (double)deltaTime;
        m_windDrift = (d > dec) ? m_windDrift * ((d - dec) / d) : glm::dvec3(0.0);
    }

    physicsBody->velocity += (m_windDrift - driftAntes);
    velocity = physicsBody->velocity;

    // ── SONDA (`HARUKA_WIND_DIAG=1`): cada 2 s, lo que el viento le hace al personaje ───────────
    // Andoni (20-09): "el viento hace que se mueva por si solo o impide el movimiento en una
    // situacion normal". Con 3,5-6 m/s el arrastre es 0,06-0,19 m/s2, muy por debajo del rozamiento
    // de 4,5: de pie no puede pasar nada... salvo que el viento que llega no sea el del log, o que
    // `grounded` parpadee y la deriva se acumule sin freno. Esta linea lo separa: viento recibido,
    // aceleracion, deriva, y el % de frames con los pies en el suelo.
    {
        static const bool s_diag = [] { const char* e = std::getenv("HARUKA_WIND_DIAG"); return e && e[0] == '1'; }();
        if (s_diag) {
            static double acc = 0.0, sumWind = 0.0, maxWind = 0.0, sumAcc = 0.0, maxAcc = 0.0, maxDrift = 0.0, maxRad = 0.0;
            static int n = 0, nGround = 0;
            acc += deltaTime; sumWind += glm::length(vAir); maxWind = std::max(maxWind, glm::length(vAir));
            sumAcc += al; maxAcc = std::max(maxAcc, al);
            maxDrift = std::max(maxDrift, glm::length(m_windDrift));
            maxRad = std::max(maxRad, std::abs(glm::dot(glm::dvec3(physicsBody->velocity), up)));
            ++n; if (grounded) ++nGround;
            if (acc >= 2.0 && n > 0) {
                std::printf("[Viento] recibido media %.2f max %.2f m/s · arrastre media %.3f max %.3f m/s2 · deriva max %.3f m/s"
                            " · pies en el suelo %d%% de %d frames · |v radial| max %.2f m/s · v cuerpo %.2f m/s\n",
                            sumWind / n, maxWind, sumAcc / n, maxAcc, maxDrift, 100 * nGround / n, n, maxRad,
                            glm::length(glm::dvec3(physicsBody->velocity)));
                acc = 0.0; sumWind = sumAcc = maxWind = maxAcc = maxDrift = maxRad = 0.0; n = nGround = 0;
            }
        }
    }
}

void Character::processInput(SDL_Window* window, float deltaTime) {
    if (!localPlayer) return;

    // Mouse look via SDL_GetRelativeMouseState — fallback for when the game
    // does not forward SDL_EVENT_MOUSE_MOTION through GameInterface::onEvent.
    if (window && SDL_GetWindowRelativeMouseMode(window)) {
        float mx = 0.f, my = 0.f;
        SDL_GetRelativeMouseState(&mx, &my);
        if (mx != 0.f || my != 0.f)
            rotate(mx, -my);
    }
    // EL STICK DERECHO: la accion "Look" (vec2, si el juego la registra) gira la vista a velocidad
    // constante —`kStickDegPerSec` grados por segundo a tope— en vez de por desplazamiento como el
    // raton. `rotate` multiplica por `mouseSensitivity`, asi que aqui se divide para que el ajuste
    // del raton no cambie la velocidad del stick.
    if (m_inputProvider.readValue && mouseSensitivity > 1e-6f) {
        const Input::ActionValue lv = m_inputProvider.readValue("Look");
        if (const glm::vec2* look = std::get_if<glm::vec2>(&lv)) {
            constexpr float kStickDegPerSec = 160.0f;
            if (look->x != 0.f || look->y != 0.f)
                rotate(look->x * kStickDegPerSec * deltaTime / mouseSensitivity,
                       look->y * kStickDegPerSec * deltaTime / mouseSensitivity);
        }
    }

    if (m_bindings.empty()) return;
    for (const auto& b : m_bindings) {
        bool fire = false;
        switch (b.trigger) {
            case ActionTrigger::Performed:
                fire = m_inputProvider.isPerformed && m_inputProvider.isPerformed(b.action);
                break;
            case ActionTrigger::Started:
                fire = m_inputProvider.isStarted   && m_inputProvider.isStarted(b.action);
                break;
            case ActionTrigger::Canceled:
                fire = m_inputProvider.isCanceled  && m_inputProvider.isCanceled(b.action);
                break;
        }
        if (!fire) continue;

        Input::ActionValue val = (m_inputProvider.readValue)
            ? m_inputProvider.readValue(b.action)
            : Input::ActionValue{ false };

        b.callback(*this, deltaTime, val);
    }

    Haruka::Rotation camOrientation = camera
        ? camera->orientation
        : Haruka::Rotation(glm::dvec3(0.0, 0.0, 0.0));

    if (onTransformChanged)
        onTransformChanged(position, camOrientation);

    // Update camera AFTER movement and rotation are applied so the sync
    // in Application::run() copies a fully-current position/orientation.
    updateCamera();
}

float Character::getSpeed() const {
    // El terreno FRENA: nieve profunda, arena, barro. Va aquí y no en cada llamada de movimiento para
    // que lo respeten todos los caminos (andar, correr, agacharse) sin poder olvidarse de uno.
    return (sprinting ? runSpeed : (crouched ? crouchSpeed : walkSpeed)) * groundSpeedFactor;
}

void Character::move(glm::vec2 input, float deltaTime) {
    Haruka::WorldPos up, surfaceForward, surfaceRight;
    Haruka::Rotation camOri = camera
        ? camera->orientation
        : Haruka::Rotation(glm::dvec3(0.0, 0.0, 0.0));
    buildSurfaceBasis(getEffectiveUp(), camOri, up, surfaceForward, surfaceRight);

    float speed = getSpeed();
    glm::dvec3 delta = surfaceForward * (double)(input.y * speed)
                     + surfaceRight   * (double)(input.x * speed);   // velocidad de locomoción (m/s, tangente)
    glm::dvec3 u = glm::dvec3(up);

    // VOLANDO manda el juego (el cuerpo está kinemático y NADIE lo integra), así que el desplazamiento
    // horizontal debe escribir la POSICIÓN como en el camino clásico. Con el camino de física solo se
    // fijaba una velocidad que nadie consumía → se ascendía (eso sí mueve la posición) pero no se andaba.
    if (m_physicsDriven && physicsBody && !flightMode) {
        // MOTOR-DRIVEN: no escribimos la posición. Fijamos la velocidad TANGENCIAL de locomoción en
        // el body y CONSERVAMOS su componente radial (gravedad/salto, que integra el motor). El motor
        // avanza el body (advance) → la posición sale de ahí, y una fuerza externa (viento, empujón,
        // magia) se suma a esa velocidad en vez de ser ignorada.
        // ⚠️ `+ m_windDrift`: sin él, andar ANULA el viento. Esta línea reescribe la tangencial
        // entera cada frame con tecla pulsada, así que el empujón del tornado que `applyWind` acaba
        // de sumar desaparecería en cuanto el jugador intentara moverse — es decir, justo cuando
        // importa. Se suma la deriva porque andar y que te arrastren pasan A LA VEZ.
        const glm::dvec3 vBody = physicsBody->velocity;
        physicsBody->velocity  = delta + m_windDrift + u * glm::dot(vBody, u);
        velocity = physicsBody->velocity;
        return;
    }

    // CLÁSICO (kinemático): el juego integra a mano → escribimos la posición.
    position += delta * (double)deltaTime;
    // Conserva la componente RADIAL de la velocidad (salto/gravedad) — solo reemplaza la
    // horizontal — para no matar el salto al moverse a la vez.
    velocity = delta + u * glm::dot(velocity, u);
}

// FRICCIÓN al soltar el movimiento (jugador físico). `move()` es un binding "Performed" → solo corre con
// tecla pulsada; al soltar, la última velocidad tangencial se quedaba y el motor la re-inyectaba cada
// frame → deslizabas sin parar. Aquí se anula SOLO la componente tangencial y se conserva la RADIAL
// (gravedad/salto en curso) → paras en seco pero sigues cayendo/subiendo con normalidad.
void Character::stopWalking() {
    if (!m_physicsDriven || !physicsBody) return;
    glm::dvec3 u = glm::dvec3(getEffectiveUp());
    const double ul = glm::length(u);
    u = (ul > 1e-6) ? u / ul : glm::dvec3(0.0, 1.0, 0.0);
    // Lo radial (gravedad/salto) y la DERIVA DEL VIENTO sobreviven; lo que se anula es la locomoción.
    // Soltar la tecla no te libra de un tornado.
    physicsBody->velocity = u * glm::dot(glm::dvec3(physicsBody->velocity), u) + m_windDrift;
    velocity = physicsBody->velocity;
}

void Character::moveForward(float amount) {
    position += Haruka::WorldPos(forward) * (double)amount;
}

void Character::moveRight(float amount) {
    position += Haruka::WorldPos(right) * (double)amount;
}

void Character::jump() {
    if (grounded && physicsBody) {
        // Impulso RADIAL (hacia fuera del planeta), no en world-Y: en una esfera el "arriba"
        // varía. La constraint del juego integra esta velocidad (gravedad) → arco de salto.
        glm::dvec3 up = glm::dvec3(getEffectiveUp());
        physicsBody->velocity = up * (double)jumpForce;
        velocity = physicsBody->velocity;
        grounded = false;
    }
}

void Character::crouch(bool enabled) {
    crouched = enabled;
}

void Character::sprint(bool enabled) {
    sprinting = enabled && !crouched;
}

void Character::rotate(float yawDelta, float pitchDelta) {
    if (!localPlayer) return;
    
    yaw += yawDelta * mouseSensitivity;
    pitch += pitchDelta * mouseSensitivity;

    if (minPitch > maxPitch) std::swap(minPitch, maxPitch);
    pitch = glm::clamp(pitch, minPitch, maxPitch);
    
    glm::vec3 direction;
    direction.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    direction.y = sin(glm::radians(pitch));
    direction.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    forward = glm::normalize(direction);
    
    right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
}

void Character::updateCamera() {
    if (!camera) return;

    glm::dvec3 up = getEffectiveUp();
    glm::dvec3 cameraPos = position + up * (double)(currentHeight * 0.9f);
    camera->position = WorldPos(cameraPos.x, cameraPos.y, cameraPos.z);

    glm::dvec3 referenceForward = glm::dvec3(0.0, 0.0, -1.0);
    if (std::abs(glm::dot(referenceForward, up)) > 0.95) {
        referenceForward = glm::dvec3(1.0, 0.0, 0.0);
    }

    glm::dvec3 tangentForward = safeNormalize(referenceForward - up * glm::dot(referenceForward, up), glm::dvec3(1.0, 0.0, 0.0));
    glm::dvec3 tangentRight = safeNormalize(glm::cross(tangentForward, up), glm::dvec3(0.0, 0.0, 1.0));

    glm::dquat yawQ = glm::angleAxis(glm::radians((double)yaw), up);
    glm::dvec3 forwardAfterYaw = glm::normalize(yawQ * tangentForward);
    glm::dvec3 rightAfterYaw = safeNormalize(glm::cross(forwardAfterYaw, up), tangentRight);

    glm::dquat pitchQ = glm::angleAxis(glm::radians((double)pitch), rightAfterYaw);
    glm::dvec3 finalForward = glm::normalize(pitchQ * forwardAfterYaw);

    glm::dvec3 finalRight = safeNormalize(glm::cross(finalForward, up), rightAfterYaw);
    glm::dvec3 finalUp = safeNormalize(glm::cross(finalRight, finalForward), up);

    glm::dmat3 basis;
    basis[0] = finalRight;
    basis[1] = finalUp;
    basis[2] = -finalForward;
    camera->orientation = glm::normalize(glm::quat_cast(basis));
}

glm::dvec3 Character::getEffectiveUp() const {
    glm::dvec3 up = safeNormalize(upDirection, glm::dvec3(0.0, 1.0, 0.0));
    if (glm::length(up) < 1e-9) {
        up = safeNormalize(position, glm::dvec3(0.0, 1.0, 0.0));
    }
    return up;
}

void Character::updateState() {
    float horizontalSpeed = glm::length(glm::vec2(velocity.x, velocity.z));

    if (flightMode) {
        if (horizontalSpeed > 0.1f)
            state = sprinting ? CharacterState::RUNNING : CharacterState::WALKING;
        else
            state = CharacterState::IDLE;
        return;
    }

    if (!grounded) {
        state = velocity.y > 0 ? CharacterState::JUMPING : CharacterState::FALLING;
    } else if (crouched) {
        state = CharacterState::CROUCHING;
    } else if (horizontalSpeed > 0.1f) {
        state = sprinting ? CharacterState::RUNNING : CharacterState::WALKING;
    } else {
        state = CharacterState::IDLE;
    }
}

void Character::checkGrounded() {
    if (flightMode) { grounded = true; return; }
    // If the game manages grounding via a surface constraint (the local player on a
    // planet), trust it — the world-Y velocity test below is wrong on a sphere/slope
    // (up is radial, not +Y) and would report FALLING while standing on a slope.
    if (m_externalGround) return;
    if (physicsBody) {
        grounded = (physicsBody->velocity.y < 0.1f && physicsBody->velocity.y > -0.1f);
    }
}

};