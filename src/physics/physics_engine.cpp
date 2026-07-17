#include "physics_engine.h"

#include <iostream>
#include <algorithm>
#include <cmath>

namespace Haruka { namespace Physics {

PhysicsEngine::PhysicsEngine() {}

PhysicsEngine::~PhysicsEngine() {}

void PhysicsEngine::addBody(std::shared_ptr<RigidBody> body) {
    bodies.push_back(body);
    if (octree) octree->insert(body);
}

void PhysicsEngine::removeBody(const std::string& name) {
    auto it = std::find_if(bodies.begin(), bodies.end(),
        [&name](const std::shared_ptr<RigidBody>& b) { return b->name == name; });
    if (it != bodies.end()) {
        bodies.erase(it);
    }
}

std::shared_ptr<RigidBody> PhysicsEngine::getBody(const std::string& name) {
    auto it = std::find_if(bodies.begin(), bodies.end(),
        [&name](const std::shared_ptr<RigidBody>& b) { return b->name == name; });
    return (it != bodies.end()) ? *it : nullptr;
}

void PhysicsEngine::update(double deltaTime) {
    integrateForces(deltaTime);
    broadPhaseAABB();
    detectCollisions();
    resolveCollisions();
    resolveStaticCollisions();
}

void PhysicsEngine::advance(double frameDt) {
    // TIMESTEP FIJO con acumulador. El motor SIEMPRE avanza en pasos de kFixedDt, sea cual sea el
    // dt de reloj. Es REQUISITO de determinismo cliente↔servidor: con dt variable, el mismo input
    // da trayectorias distintas según los fps → cliente y DGS discreparían y la validación fallaría.
    // El resto (frameDt no múltiplo exacto) se acumula para el frame siguiente.
    m_accum += frameDt;
    // Cota anti-espiral: si el frame se congela (breakpoint, hitch), no intentar recuperar segundos
    // de simulación de golpe (bloquearía más) — se descarta el exceso.
    if (m_accum > kMaxAccum) m_accum = kMaxAccum;
    while (m_accum >= kFixedDt) {
        update(kFixedDt);
        m_accum -= kFixedDt;
    }
}

void PhysicsEngine::integrateForces(double dt) {
    // Nivel del mar del planeta activo (esfera de radio seaR). La TIERRA siempre está por
    // encima del nivel del mar → un cuerpo por debajo de esta esfera está en una cuenca
    // oceánica = sumergido. Se usa para el empuje de Arquímedes de los cuerpos dinámicos.
    bool       haveSea = false;
    glm::dvec3 seaCenter(0.0);
    double     seaR = 0.0;
    if (m_world && m_world->hasActivePlanet()) {
        haveSea   = true;
        seaCenter = m_world->activePlanetCenter();
        seaR      = m_world->activePlanetRadius();
    }

    for (auto& body : bodies) {
        if (body->isKinematic) continue;

        // Gravedad RADIAL hacia el planeta (antes era plana -Y, que solo valía cerca del polo). La
        // suma newtoniana de los cuerpos vía calculateGravityAtPosition da la dirección y magnitud
        // reales; sin m_world cae a (0,-1,0)·9.81. Es lo que permite que un cuerpo dinámico "caiga
        // hacia el suelo" en cualquier punto del planeta.
        glm::dvec3 gDir;
        double gMag = calculateGravityAtPosition(body->position, gDir);
        body->acceleration = gDir * gMag;

        // Velocity Verlet
        body->velocity += body->acceleration * dt;

        // Arrastre AERODINÁMICO (solo cuerpos dinámicos = conjunto acotado cerca del
        // juego → O(cuerpos), sin coste global). Resistencia del aire (amortigua hacia
        // 0) + empuje del viento (cuadrático con la velocidad relativa). Suave.
        body->velocity *= (1.0 - std::min(m_airDamp * dt, 0.5));
        glm::dvec3 vRel = m_wind - body->velocity;
        double rel = glm::length(vRel);
        if (rel > 1e-6)
            body->velocity += vRel * std::min(m_windCoef * rel * dt, 0.20);

        // --- BUOYANCY (empuje de Arquímedes) + arrastre de agua para cuerpos SUMERGIDOS ---
        // f = fracción sumergida: 0 cuando el cuerpo apenas toca la superficie por arriba
        // (dist = seaR+radio), 1 cuando está totalmente bajo el agua (dist ≤ seaR−radio).
        // Empuje hacia el radial +up escala con f y con la razón de densidades (buoyRatio>1 →
        // flota); arrastre fuerte amortigua la velocidad → el objeto se ASIENTA flotando en la
        // superficie (equilibrio a f≈1/buoyRatio) en vez de oscilar. up radial ≈ world-up cerca
        // del jugador (donde la gravedad plana -Y ya apunta hacia el planeta).
        if (haveSea && body->radius > 1e-4) {
            glm::dvec3 rel2 = body->position - seaCenter;
            double dist = glm::length(rel2);
            double r    = body->radius;
            if (dist > 1e-6 && dist < seaR + r) {
                glm::dvec3 up = rel2 / dist;
                double f = glm::clamp((seaR + r - dist) / (2.0 * r), 0.0, 1.0);
                // SPLASH: al CRUZAR la superficie hacia dentro (aire→agua) con velocidad de entrada
                // apreciable → dispara el callback UNA vez (no cada frame, por el flag inWater). El
                // juego emite partículas PBF ahí → el impacto salpica.
                if (f > 0.05 && !body->inWater) {
                    double vDown = -glm::dot(body->velocity, up); // componente hacia el agua (m/s)
                    if (vDown > 1.5 && m_onWaterEntry)
                        m_onWaterEntry(seaCenter + up * seaR, body->velocity, r);
                    body->inWater = true;
                }
                if (f > 0.0) {
                    double g = glm::length(gravity);
                    const double buoyRatio = 1.1;                 // agua/objeto (>1 = flota)
                    body->velocity += up * (f * g * buoyRatio * dt);   // Arquímedes
                    body->velocity -= body->velocity * (1.0 - std::exp(-3.0 * f * dt)); // arrastre
                }
            } else {
                body->inWater = false; // fuera del agua → rearma el splash para la próxima entrada
            }
        }

        body->position += body->velocity * dt;

        // --- GROUND-SNAP contra el TERRENO ------------------------------------------------------
        // El suelo está en radio = planetRadius + altura del terreno + radio del cuerpo. Si el cuerpo
        // cayó por debajo, se sube a la superficie y se le quita la componente de velocidad que
        // apunta HACIA DENTRO (la tangencial se conserva → puede deslizar/andar). El terreno lo da
        // m_world (cliente: la malla F10; servidor: el sampler analítico) → misma superficie que la
        // que se ve/valida. Sustituye al "surface constraint" que el juego hacía a mano.
        if (haveSea && body->radius > 1e-4) {
            const glm::dvec3 rel = body->position - seaCenter;
            const double dist = glm::length(rel);
            if (dist > 1e-6) {
                const glm::dvec3 up = rel / dist;
                if (body->shape == RigidBody::Shape::Box && m_world) {
                    // COLISIÓN CONTRA EL TERRENO POR LOS VÉRTICES DE LA FORMA REAL. Si el cuerpo define
                    // sus `points` (casco convexo del objeto), se prueba cada uno; si no, la caja usa sus
                    // 8 esquinas. Así la colisión sigue la FORMA del objeto (no una esfera/caja fija). El
                    // cuerpo se apoya en su cara/vértices y VUELCA sobre la arista si el centro de masa se
                    // sale del apoyo. ⚠️ ESTABILIDAD (antes "flotaba/giraba"): posición corregida por la
                    // penetración MÁXIMA una vez (no sumada → no se autolanza), impulsos REPARTIDOS entre
                    // contactos, y amortiguación angular fuerte + asentamiento al descansar lento.
                    const glm::dvec3 comW = body->orientation * body->comOffset;
                    const double invM = 1.0 / body->mass;
                    const double havg = (body->halfExtents.x + body->halfExtents.y + body->halfExtents.z) / 3.0;
                    const double invI = 1.0 / ((1.0 / 6.0) * body->mass * (2.0 * havg) * (2.0 * havg) + 1e-9);
                    glm::dvec3 hit[32]; int nh = 0; double maxPen = 0.0;
                    auto test = [&](const glm::dvec3& local) {
                        const glm::dvec3 corner = body->position + body->orientation * local;
                        const double cdist = glm::length(corner - seaCenter);
                        if (cdist < 1e-6) return;
                        const double pen = (seaR + m_world->terrainHeightAt(corner)) - cdist;
                        if (pen > 0.0 && nh < 32) { hit[nh++] = corner; maxPen = std::max(maxPen, pen); }
                    };
                    if (!body->points.empty()) {
                        for (const glm::dvec3& p : body->points) test(p);
                    } else {
                        for (int sx = -1; sx <= 1; sx += 2)
                        for (int sy = -1; sy <= 1; sy += 2)
                        for (int sz = -1; sz <= 1; sz += 2)
                            test(glm::dvec3(sx * body->halfExtents.x, sy * body->halfExtents.y, sz * body->halfExtents.z));
                    }
                    if (nh > 0) {
                        body->position += up * (maxPen * 0.8);                    // sacar del suelo UNA vez
                        const double relax = 1.0 / (double)nh;                    // repartir entre esquinas
                        for (int k = 0; k < nh; ++k) {
                            const glm::dvec3 rc = hit[k] - (body->position + comW);
                            const glm::dvec3 vc = body->velocity + glm::cross(body->angularVel, rc);
                            const double vn = glm::dot(vc, up);
                            if (vn >= 0.0) continue;
                            const glm::dvec3 rcxn = glm::cross(rc, up);
                            const double kn = invM + glm::dot(up, glm::cross(rcxn * invI, rc));
                            const double e = (vn < -1.5) ? 0.3 : 0.0;              // BOTE solo en impactos fuertes
                            const double jn = (-(1.0 + e) * vn / std::max(kn, 1e-9)) * relax;   // relajado
                            const glm::dvec3 P = up * jn;
                            body->velocity   += P * invM;
                            body->angularVel += glm::cross(rc, P) * invI;
                            const glm::dvec3 vt = vc - up * vn;                     // rozamiento tangencial
                            const double vtl = glm::length(vt);
                            if (vtl > 1e-6) {
                                const glm::dvec3 t = vt / vtl;
                                const glm::dvec3 rcxt = glm::cross(rc, t);
                                const double kt = invM + glm::dot(t, glm::cross(rcxt * invI, rc));
                                const double jt = glm::clamp((-vtl / std::max(kt, 1e-9)) * relax, -0.7 * jn, 0.7 * jn);
                                const glm::dvec3 Pt = t * jt;
                                body->velocity   += Pt * invM;
                                body->angularVel += glm::cross(rc, Pt) * invI;
                            }
                        }
                        // Amortiguación angular: fuerte, y AÚN más cuando descansa girando lento →
                        // ASIENTA en vez de rotar sobre sí mismo eternamente (lo que veías).
                        const double slow = glm::length(body->angularVel) < 0.6 ? 8.0 : 3.0;
                        body->angularVel *= std::max(0.0, 1.0 - slow * dt);
                    }
                } else {
                const double terrainM = m_world ? m_world->terrainHeightAt(body->position) : 0.0;
                const double floorDist = seaR + terrainM + body->radius;
                if (dist < floorDist) {
                    body->position = seaCenter + up * floorDist;          // sube al suelo
                    const double vIn = glm::dot(body->velocity, up);      // <0 = cayendo hacia dentro
                    if (vIn < 0.0) {
                        const double e = (vIn < -1.5) ? 0.3 : 0.0;        // BOTE solo en impactos fuertes (no en reposo)
                        body->velocity -= up * vIn * (1.0 + e);           // anula lo radial + rebota una fracción
                    }

                    // DESLIZAMIENTO POR PENDIENTE. La componente de la gravedad TANGENTE al terreno
                    // empuja cuesta abajo, pero solo en cuestas más empinadas que el ÁNGULO DE REPOSO
                    // (kReposeCos): así los objetos sueltos ruedan por las laderas fuertes y se quedan
                    // en lo llano — y el jugador NO resbala en pendientes suaves. La normal sale de
                    // muestrear el MISMO campo del suelo (terrainHeightAt) en dos tangentes → coincide
                    // con la superficie que se ve/colisiona. + rozamiento tangencial (los sueltos frenan).
                    constexpr double kReposeCos      = 0.82;   // cos(~35°): más empinado → desliza
                    constexpr double kGroundFriction = 2.5;    // 1/s de rozamiento tangencial en el suelo
                    glm::dvec3 t1 = glm::cross(up, std::abs(up.y) < 0.99 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
                    const double t1len = glm::length(t1);
                    if (m_world && t1len > 1e-9) {
                        t1 /= t1len;
                        const glm::dvec3 t2 = glm::cross(up, t1);
                        const double e  = std::max(0.25, body->radius);
                        const double dh1 = (m_world->terrainHeightAt(body->position + t1 * e)
                                          - m_world->terrainHeightAt(body->position - t1 * e)) / (2.0 * e);
                        const double dh2 = (m_world->terrainHeightAt(body->position + t2 * e)
                                          - m_world->terrainHeightAt(body->position - t2 * e)) / (2.0 * e);
                        const glm::dvec3 N = glm::normalize(up - t1 * dh1 - t2 * dh2);
                        if (glm::dot(N, up) < kReposeCos) {              // cuesta > reposo → deslizar
                            const glm::dvec3 gvec = gDir * gMag;         // gravedad (radial)
                            const glm::dvec3 tang = gvec - glm::dot(gvec, N) * N;   // proyección cuesta abajo
                            body->velocity += tang * dt;
                        }
                        const glm::dvec3 vTang = body->velocity - glm::dot(body->velocity, up) * up;
                        body->velocity -= vTang * std::min(kGroundFriction * dt, 1.0);
                    }
                    // VUELCO por BALANCE: si el centro de masa está DESCENTRADO (comOffset≠0), apoyado
                    // la gravedad hace PALANCA → un torque que gira el cuerpo hacia donde pesa (vuelca
                    // por un lado y no por otro). Inercia ∝ masa·radio² → los más pesados/grandes
                    // vuelcan más despacio. comOffset=0 (equilibrado) → torque nulo, no vuelca.
                    if (glm::dot(body->comOffset, body->comOffset) > 1e-12) {
                        const glm::dvec3 comW   = body->orientation * body->comOffset;   // offset → mundo
                        const glm::dvec3 torque = glm::cross(comW, gDir * (gMag * body->mass));
                        const double     I      = 0.4 * body->mass * body->radius * body->radius + 1e-6;
                        body->angularVel += (torque / I) * dt;
                        body->angularVel *= std::max(0.0, 1.0 - 1.5 * dt);              // rozamiento angular
                    }
                }
                }   // cierra el else (rama ESFERA)
            }
        }

        // Integrar la ORIENTACIÓN desde la velocidad angular (cualquier cuerpo que gire; con angularVel=0
        // no hay cambio). Cuaternión: q ← normalize(q + ½·ω·q·dt).
        if (glm::dot(body->angularVel, body->angularVel) > 1e-14) {
            const glm::dquat w(0.0, body->angularVel.x, body->angularVel.y, body->angularVel.z);
            body->orientation = glm::normalize(body->orientation + 0.5 * w * body->orientation * dt);
        }
    }
}

namespace {
    // Vértices del mundo de un cuerpo con forma: su casco `points` si lo tiene, si no las 8 esquinas.
    inline int shapeVertices(const RigidBody* b, glm::dvec3 out[32]) {
        int n = 0;
        if (!b->points.empty()) {
            for (const auto& p : b->points) if (n < 32) out[n++] = b->position + b->orientation * p;
        } else {
            for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) for (int sz = -1; sz <= 1; sz += 2)
                out[n++] = b->position + b->orientation * glm::dvec3(sx * b->halfExtents.x, sy * b->halfExtents.y, sz * b->halfExtents.z);
        }
        return n;
    }
    // ¿Punto p (mundo) DENTRO de la caja? outN = normal de cara SALIENTE (mundo), pen = profundidad (eje mínimo).
    inline bool pointInBox(const glm::dvec3& p, const RigidBody* box, glm::dvec3& outN, double& pen) {
        const glm::dvec3 lp = glm::conjugate(box->orientation) * (p - box->position);
        const glm::dvec3 h = box->halfExtents;
        const double dx = h.x - std::abs(lp.x), dy = h.y - std::abs(lp.y), dz = h.z - std::abs(lp.z);
        if (dx < 0.0 || dy < 0.0 || dz < 0.0) return false;               // fuera
        glm::dvec3 ln;
        if (dx <= dy && dx <= dz)      { ln = glm::dvec3(lp.x < 0 ? -1 : 1, 0, 0); pen = dx; }
        else if (dy <= dz)             { ln = glm::dvec3(0, lp.y < 0 ? -1 : 1, 0); pen = dy; }
        else                           { ln = glm::dvec3(0, 0, lp.z < 0 ? -1 : 1); pen = dz; }
        outN = box->orientation * ln;                                     // normal saliente en el mundo
        return true;
    }
}

void PhysicsEngine::detectCollisions() {
    collisions.clear();

    // NARROW-PHASE POR FORMA (nada de radio envolvente para la colisión, salvo esfera↔esfera, que ES
    // su forma real). El radio solo lo usa el BROAD-PHASE para descartar pares.
    //  · esfera↔esfera: 1 contacto por radios.
    //  · caja↔caja: los VÉRTICES de cada una dentro de la otra → un contacto por vértice (apila real).
    //  · esfera↔caja: el punto más cercano de la caja al centro de la esfera.
    auto narrow = [&](RigidBody* a, RigidBody* b) {
        const bool aBox = (a->shape == RigidBody::Shape::Box);
        const bool bBox = (b->shape == RigidBody::Shape::Box);
        if (!aBox && !bBox) {                                   // esfera ↔ esfera
            const glm::dvec3 d = b->position - a->position;
            const double dist = glm::length(d), minD = a->radius + b->radius;
            if (dist < minD && dist > 1e-9) {
                CollisionInfo c; c.bodyA = a; c.bodyB = b; c.penetration = minD - dist;
                c.normal = d / dist; c.point = a->position + c.normal * a->radius;
                collisions.push_back(c);
            }
            return;
        }
        if (aBox && bBox) {                                    // caja ↔ caja (por vértices, ambos sentidos)
            glm::dvec3 v[32], nOut; double pen;
            const int na = shapeVertices(a, v);
            for (int i = 0; i < na; ++i) if (pointInBox(v[i], b, nOut, pen)) {
                CollisionInfo c; c.bodyA = a; c.bodyB = b; c.penetration = pen;
                c.normal = -nOut; c.point = v[i]; collisions.push_back(c);   // saca el vértice de A fuera de B
            }
            const int nb = shapeVertices(b, v);
            for (int i = 0; i < nb; ++i) if (pointInBox(v[i], a, nOut, pen)) {
                CollisionInfo c; c.bodyA = a; c.bodyB = b; c.penetration = pen;
                c.normal = nOut; c.point = v[i]; collisions.push_back(c);    // saca el vértice de B fuera de A
            }
            return;
        }
        // esfera ↔ caja
        RigidBody* s = aBox ? b : a;                            // la esfera
        RigidBody* x = aBox ? a : b;                            // la caja
        const glm::dvec3 lc = glm::conjugate(x->orientation) * (s->position - x->position);
        const glm::dvec3 cl = glm::clamp(lc, -x->halfExtents, x->halfExtents);
        glm::dvec3 nOut; double pen;
        if (cl == lc) {                                        // centro DENTRO de la caja
            if (!pointInBox(s->position, x, nOut, pen)) return;
            pen += s->radius;
        } else {
            const glm::dvec3 wc = x->position + x->orientation * cl;
            const glm::dvec3 delta = s->position - wc;
            const double dist = glm::length(delta);
            if (dist >= s->radius || dist < 1e-9) return;
            nOut = delta / dist; pen = s->radius - dist;
        }
        CollisionInfo c; c.bodyA = a; c.bodyB = b; c.penetration = pen;
        c.point = x->position + x->orientation * cl;
        c.normal = (s == a) ? -nOut : nOut;                    // nOut va de la caja hacia la esfera
        collisions.push_back(c);
    };

    if (!octree) {
        for (size_t i = 0; i < bodies.size(); ++i)
            for (size_t j = i + 1; j < bodies.size(); ++j)
                narrow(bodies[i].get(), bodies[j].get());
    } else {
        for (auto& body : bodies) {
            std::vector<std::shared_ptr<RigidBody>> nearby;
            octree->getNearbodies(body, nearby);
            for (auto& other : nearby)
                if (body.get() < other.get())                  // cada par UNA vez (orden estable por puntero)
                    narrow(body.get(), other.get());
        }
    }
}

void PhysicsEngine::resolveCollisions() {
    // Resolución por IMPULSOS en el PUNTO DE CONTACTO real (col.point): separa el solape, intercambia
    // MOMENTO (lineal) y aplica impulso ANGULAR con el brazo al centro de masa → los cuerpos se empujan
    // y GIRAN al chocar según DÓNDE se tocan (una caja golpeada en una esquina vuelca). Inercia POR
    // FORMA (caja vs esfera). Rozamiento tangencial en el cono de Coulomb.
    constexpr double kFriction = 0.5;
    auto invInertia = [](const RigidBody* b) -> double {
        if (b->isKinematic) return 0.0;
        double I;
        if (b->shape == RigidBody::Shape::Box) {
            const double havg = (b->halfExtents.x + b->halfExtents.y + b->halfExtents.z) / 3.0;
            I = (1.0 / 6.0) * b->mass * (2.0 * havg) * (2.0 * havg);
        } else {
            I = 0.4 * b->mass * b->radius * b->radius;
        }
        return 1.0 / (I + 1e-9);
    };
    for (auto& col : collisions) {
        RigidBody* A = col.bodyA; RigidBody* B = col.bodyB;
        const bool aKin = A->isKinematic, bKin = B->isKinematic;
        if (aKin && bKin) continue;
        const glm::dvec3 n = col.normal;                        // de A hacia B
        const double invMa = aKin ? 0.0 : 1.0 / A->mass;
        const double invMb = bKin ? 0.0 : 1.0 / B->mass;
        const double invSum = invMa + invMb;
        if (invSum <= 0.0) continue;

        // (1) Separación del solape (Baumgarte 0.8: suave, para no explotar con varios contactos/par).
        const double corr = (col.penetration * 0.8) / invSum;
        A->position -= n * (corr * invMa);
        B->position += n * (corr * invMb);

        // (2) Impulso en el punto de contacto REAL, con inercia por forma.
        const double invIa = invInertia(A), invIb = invInertia(B);
        const glm::dvec3 rA = col.point - (A->position + A->orientation * A->comOffset);
        const glm::dvec3 rB = col.point - (B->position + B->orientation * B->comOffset);
        const glm::dvec3 vRel = (B->velocity + glm::cross(B->angularVel, rB))
                              - (A->velocity + glm::cross(A->angularVel, rA));
        const double vn = glm::dot(vRel, n);
        if (vn < 0.0) {                                         // se acercan → responder
            const double e = (vn < -1.5) ? 0.2 : 0.0;          // bote solo en impactos fuertes
            const glm::dvec3 rAxn = glm::cross(rA, n), rBxn = glm::cross(rB, n);
            const double kn = invSum + glm::dot(n, glm::cross(rAxn * invIa, rA))
                                     + glm::dot(n, glm::cross(rBxn * invIb, rB));
            const double jn = -(1.0 + e) * vn / std::max(kn, 1e-9);
            const glm::dvec3 P = n * jn;
            A->velocity   -= P * invMa;                B->velocity   += P * invMb;
            A->angularVel -= glm::cross(rA, P) * invIa; B->angularVel += glm::cross(rB, P) * invIb;

            // (3) Rozamiento tangencial (cono de Coulomb) → frena el deslizamiento y da GIRO al chocar.
            const glm::dvec3 vt = vRel - n * vn;
            const double vtl = glm::length(vt);
            if (vtl > 1e-6) {
                const glm::dvec3 t = vt / vtl;
                const glm::dvec3 rAxt = glm::cross(rA, t), rBxt = glm::cross(rB, t);
                const double kt = invSum + glm::dot(t, glm::cross(rAxt * invIa, rA))
                                         + glm::dot(t, glm::cross(rBxt * invIb, rB));
                const double jt = glm::clamp(-vtl / std::max(kt, 1e-9), -kFriction * jn, kFriction * jn);
                const glm::dvec3 Pt = t * jt;
                A->velocity   -= Pt * invMa;                B->velocity   += Pt * invMb;
                A->angularVel -= glm::cross(rA, Pt) * invIa; B->angularVel += glm::cross(rB, Pt) * invIb;
            }
        }
    }
}

void PhysicsEngine::addStaticBox(const glm::dvec3& center, const glm::dvec3& halfExtents) {
    staticBoxes.push_back({ center - halfExtents, center + halfExtents });
}

void PhysicsEngine::clearStaticBoxes() {
    staticBoxes.clear();
}

void PhysicsEngine::addPlacedOBB(const glm::dvec3& center, const glm::dvec3& halfExtents, const glm::dmat3& rot) {
    placedOBBs.push_back({ center, halfExtents, rot });
}

void PhysicsEngine::clearPlacedOBBs() {
    placedOBBs.clear();
}

void PhysicsEngine::addPropOBB(const glm::dvec3& center, const glm::dvec3& halfExtents, const glm::dmat3& rot) {
    propOBBs.push_back({ center, halfExtents, rot });
}

void PhysicsEngine::clearPropOBBs() {
    propOBBs.clear();
}

glm::dvec3 PhysicsEngine::resolveSphere(const glm::dvec3& center0, double radius,
                                        const glm::dvec3& up, bool& grounded) const {
    glm::dvec3 center = center0;
    auto process = [&](const std::vector<StaticOBB>& list) {
        for (const auto& b : list) {
            // Broad-phase: salta cajas lejanas (clave con cientos de props).
            glm::dvec3 dd = center - b.center;
            double maxR = radius + glm::length(b.halfExtents) + 0.5;
            if (glm::dot(dd, dd) > maxR * maxR) continue;

            // Centro de la esfera al espacio LOCAL del OBB (rot ortonormal → inv = transpose).
            glm::dvec3 lp = glm::transpose(b.rot) * (center - b.center);
            const glm::dvec3& he = b.halfExtents;
            glm::dvec3 cp = glm::clamp(lp, -he, he);
            glm::dvec3 d  = lp - cp;
            double dist2  = glm::dot(d, d);

            glm::dvec3 newlp = lp;
            if (dist2 <= 1e-12) {                       // centro DENTRO → empuja por la cara más cercana
                double px = he.x - std::abs(lp.x);
                double py = he.y - std::abs(lp.y);
                double pz = he.z - std::abs(lp.z);
                double m  = std::min({ px, py, pz });
                if      (m == px) newlp.x = (lp.x >= 0.0 ? he.x + radius : -he.x - radius);
                else if (m == py) newlp.y = (lp.y >= 0.0 ? he.y + radius : -he.y - radius);
                else              newlp.z = (lp.z >= 0.0 ? he.z + radius : -he.z - radius);
            } else if (dist2 < radius * radius) {       // la esfera roza la caja → empuja por la normal
                double dist = std::sqrt(dist2);
                newlp = cp + (dist > 1e-9 ? d / dist : glm::dvec3(0, 1, 0)) * radius;
            } else {
                continue;                               // no toca
            }

            glm::dvec3 newCenter = b.center + b.rot * newlp;
            if (glm::dot(newCenter - center, up) > 0.3 * radius) grounded = true; // apoyado encima
            center = newCenter;
        }
    };
    process(placedOBBs);
    process(propOBBs);
    return center;
}

void PhysicsEngine::broadPhaseAABB() {
    if (!octree || bodies.empty()) return;
    // Rebuild the octree each step so moved bodies are found correctly.
    // Each body is re-inserted using its updated position from integrateForces().
    for (auto& body : bodies) {
        octree->remove(body);
        octree->insert(body);
    }
}

void PhysicsEngine::resolveStaticCollisions() {
    for (auto& body : bodies) {
        if (body->isKinematic) continue;

        for (const auto& box : staticBoxes) {
            // Sphere center (body->position IS the sphere center)
            const glm::dvec3& c = body->position;
            const double      r = body->radius;

            // Closest point on AABB to sphere center
            glm::dvec3 closest = glm::clamp(c, box.bmin, box.bmax);
            glm::dvec3 diff    = c - closest;
            double dist = glm::length(diff);

            double penetration = r - dist;
            if (penetration <= 0.0) continue;

            glm::dvec3 normal;
            if (dist < 1e-9) {
                // Center is inside the box — push out on the minimum-penetration axis
                double px = std::min(c.x - box.bmin.x, box.bmax.x - c.x);
                double py = std::min(c.y - box.bmin.y, box.bmax.y - c.y);
                double pz = std::min(c.z - box.bmin.z, box.bmax.z - c.z);
                if (px <= py && px <= pz)
                    normal = (c.x < (box.bmin.x + box.bmax.x) * 0.5) ? glm::dvec3(-1,0,0) : glm::dvec3(1,0,0);
                else if (py <= px && py <= pz)
                    normal = (c.y < (box.bmin.y + box.bmax.y) * 0.5) ? glm::dvec3(0,-1,0) : glm::dvec3(0,1,0);
                else
                    normal = (c.z < (box.bmin.z + box.bmax.z) * 0.5) ? glm::dvec3(0,0,-1) : glm::dvec3(0,0,1);
                penetration = r + std::min({px, py, pz});
            } else {
                normal = diff / dist;
            }

            // Push body out of the box
            body->position += normal * penetration;

            // Cancel velocity component directed into the surface
            double vn = glm::dot(body->velocity, normal);
            if (vn < 0.0) body->velocity -= normal * vn;
        }
    }
}

double PhysicsEngine::calculateGravityAtPosition(const glm::dvec3& worldPos, glm::dvec3& outGravityDir) {
    if (!m_world) {
        outGravityDir = glm::dvec3(0.0, -1.0, 0.0);
        return 9.81;  // Default Earth gravity
    }

    glm::dvec3 totalGravity = glm::dvec3(0.0);

    // Sum gravity from all celestial bodies
    const auto& bodies = m_world->gravBodies();
    for (const auto& body : bodies) {
        totalGravity += calculateGravityContribution(body, worldPos);
    }
    
    double magnitude = glm::length(totalGravity);
    if (magnitude > 1e-6) {
        outGravityDir = glm::normalize(totalGravity);
    } else {
        outGravityDir = glm::dvec3(0.0, -1.0, 0.0);
        magnitude = 0.0;
    }
    
    return magnitude;
}

glm::dvec3 PhysicsEngine::calculateGravityContribution(const GravBody& body, const glm::dvec3& worldPos) {
    // Calculate vector from test position to body
    glm::dvec3 delta = body.worldPos - worldPos;
    double distance = glm::length(delta);
    
    // Avoid division by zero
    if (distance < 1e-6) {
        return glm::dvec3(0.0);
    }
    
    // Newton's universal law of gravitation: F = G * m1 * m2 / r²
    // Acceleration: a = F / m1 = G * m2 / r²
    double acceleration = gravitationalConstant * body.mass / (distance * distance);
    glm::dvec3 direction = glm::normalize(delta);
    
    return direction * acceleration;
}

bool PhysicsEngine::checkTerrainCollision(
    const glm::dvec3& worldPos,
    glm::dvec3* outCollisionPoint,
    glm::dvec3* outCollisionNormal) {
    // (Fase 1) El ground-snap real irá aquí, usando m_world->terrainHeightAt. Hoy es un stub
    // (nunca se usaba: el ground se hacía analíticamente en el juego). Desacoplado ya del engine.
    (void)worldPos; (void)outCollisionPoint; (void)outCollisionNormal;
    return false;
}

void PhysicsEngine::applyGravity(const glm::dvec3& worldPos, double deltaTime, glm::dvec3& inOutVelocity) {
    glm::dvec3 gravityDir;
    double gravityMagnitude = calculateGravityAtPosition(worldPos, gravityDir);
    
    // Apply gravitational acceleration: v += a * dt
    glm::dvec3 gravityAcceleration = gravityDir * gravityMagnitude;
    inOutVelocity += gravityAcceleration * deltaTime;
}

}} // namespace Haruka::Physics
