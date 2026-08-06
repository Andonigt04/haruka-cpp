#pragma once

#include "proc_graph.h"
#include <algorithm>

namespace Haruka { namespace Tools { namespace ProcGraph {

// ===========================================================================
// Constant value node
// ===========================================================================
class ConstNode : public Node {
public:
    explicit ConstNode(float value = 0, const std::string& label = "Value")
        : m_value(value), m_label(label) {}

    void setValue(float v) { m_value = v; }

    std::string name() const override { return "Const"; }
    std::vector<SocketDesc> inputs() const override { return {}; }
    std::vector<SocketDesc> outputs() const override {
        return {{m_label, DataType::Float, Value::Float(m_value)}};
    }
    void evaluate(const Value*, int, Value* outputs, int,
                  float, float, float) override {
        outputs[0] = Value::Float(m_value);
    }

private:
    float m_value;
    std::string m_label;
};

// ===========================================================================
// Binary math node (Add, Subtract, Multiply, Divide, Min, Max, Power)
// ===========================================================================
enum class MathOp {
    Add, Subtract, Multiply, Divide,
    Min, Max, Power, ATan2,
    Step, SmoothStep,
};

class MathNode : public Node {
public:
    explicit MathNode(MathOp op = MathOp::Add)
        : m_op(op) {}

    void setOp(MathOp op) { m_op = op; }

    std::string name() const override {
        switch (m_op) {
            case MathOp::Add:       return "Add";
            case MathOp::Subtract:  return "Subtract";
            case MathOp::Multiply:  return "Multiply";
            case MathOp::Divide:    return "Divide";
            case MathOp::Min:       return "Min";
            case MathOp::Max:       return "Max";
            case MathOp::Power:     return "Power";
            case MathOp::ATan2:     return "ATan2";
            case MathOp::Step:      return "Step";
            case MathOp::SmoothStep: return "SmoothStep";
        }
        return "Math";
    }

    std::vector<SocketDesc> inputs() const override {
        return {{"A", DataType::Float}, {"B", DataType::Float}};
    }

    std::vector<SocketDesc> outputs() const override {
        return {{"Result", DataType::Float}};
    }

    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float, float, float) override {
        float a = inputs[0].asFloat();
        float b = inputs[1].asFloat();
        float r = 0;
        switch (m_op) {
            case MathOp::Add:       r = a + b; break;
            case MathOp::Subtract:  r = a - b; break;
            case MathOp::Multiply:  r = a * b; break;
            case MathOp::Divide:    r = b != 0 ? a / b : 0; break;
            case MathOp::Min:       r = std::min(a, b); break;
            case MathOp::Max:       r = std::max(a, b); break;
            case MathOp::Power:     r = std::pow(a, b); break;
            case MathOp::ATan2:     r = std::atan2(a, b); break;
            case MathOp::Step:      r = b < a ? 0.0f : 1.0f; break;
            case MathOp::SmoothStep: {
                float t = std::clamp((b - a) / (1.0f - a), 0.0f, 1.0f);
                r = t * t * (3 - 2 * t);
                break;
            }
        }
        outputs[0] = Value::Float(r);
    }

private:
    MathOp m_op;
};

// ===========================================================================
// Unary math node (Abs, Negate, Sin, Cos, Tan, Sqrt, Floor, Ceil, Round, Fract, Log, Exp)
// ===========================================================================
enum class UnaryOp { Abs, Negate, Sin, Cos, Tan, Sqrt, Floor, Ceil, Round, Fract, Log, Exp, Normalize };

class UnaryNode : public Node {
public:
    explicit UnaryNode(UnaryOp op = UnaryOp::Abs) : m_op(op) {}

    std::string name() const override {
        switch (m_op) {
            case UnaryOp::Abs:   return "Abs";
            case UnaryOp::Negate: return "Negate";
            case UnaryOp::Sin:   return "Sin";
            case UnaryOp::Cos:   return "Cos";
            case UnaryOp::Tan:   return "Tan";
            case UnaryOp::Sqrt:  return "Sqrt";
            case UnaryOp::Floor: return "Floor";
            case UnaryOp::Ceil:  return "Ceil";
            case UnaryOp::Round: return "Round";
            case UnaryOp::Fract: return "Fract";
            case UnaryOp::Log:   return "Log";
            case UnaryOp::Exp:   return "Exp";
            case UnaryOp::Normalize: return "Normalize";
        }
        return "Unary";
    }

    std::vector<SocketDesc> inputs() const override {
        return {{"Value", DataType::Float}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Result", DataType::Float}};
    }
    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float, float, float) override {
        float v = inputs[0].asFloat();
        float r = 0;
        switch (m_op) {
            case UnaryOp::Abs:   r = std::abs(v); break;
            case UnaryOp::Negate: r = -v; break;
            case UnaryOp::Sin:   r = std::sin(v); break;
            case UnaryOp::Cos:   r = std::cos(v); break;
            case UnaryOp::Tan:   r = std::tan(v); break;
            case UnaryOp::Sqrt:  r = v >= 0 ? std::sqrt(v) : 0; break;
            case UnaryOp::Floor: r = std::floor(v); break;
            case UnaryOp::Ceil:  r = std::ceil(v); break;
            case UnaryOp::Round: r = std::round(v); break;
            case UnaryOp::Fract: r = v - std::floor(v); break;
            case UnaryOp::Log:   r = v > 0 ? std::log(v) : 0; break;
            case UnaryOp::Exp:   r = std::exp(v); break;
            case UnaryOp::Normalize: r = std::clamp(v, 0.0f, 1.0f); break;
        }
        outputs[0] = Value::Float(r);
    }

private:
    UnaryOp m_op;
};

// ===========================================================================
// Blend node — mix between two values by a factor
// ===========================================================================
enum class BlendMode {
    Mix, Add, Subtract, Multiply, Screen, Overlay, SoftLight, Burn, Dodge
};

class BlendNode : public Node {
public:
    explicit BlendNode(BlendMode mode = BlendMode::Mix) : m_mode(mode) {}

    std::string name() const override { return "Blend"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"A", DataType::Float}, {"B", DataType::Float}, {"Factor", DataType::Float}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Result", DataType::Float}};
    }
    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float, float, float) override {
        float a = inputs[0].asFloat();
        float b = inputs[1].asFloat();
        float f = inputs[2].asFloat();
        float r = a;
        switch (m_mode) {
            case BlendMode::Mix:       r = a * (1 - f) + b * f; break;
            case BlendMode::Add:       r = a + b * f; break;
            case BlendMode::Subtract:  r = a - b * f; break;
            case BlendMode::Multiply:  r = a * (1 - f + b * f); break;
            case BlendMode::Screen:    r = 1 - (1 - a) * (1 - b * f); break;
            case BlendMode::Overlay:   r = a < 0.5 ? 2 * a * b * f : 1 - 2 * (1 - a) * (1 - b * f); break;
            case BlendMode::SoftLight: { float t = b * f; r = (1 - 2 * t) * a * a + 2 * t * a; } break;
            case BlendMode::Burn:      r = 1 - (1 - a) / (b * f + 0.001f); break;
            case BlendMode::Dodge:     r = a / (1 - b * f + 0.001f); break;
        }
        outputs[0] = Value::Float(std::clamp(r, 0.0f, 1.0f));
    }

private:
    BlendMode m_mode;
};

// ===========================================================================
// Color ramp — maps a float input through a series of control points
// ===========================================================================
struct ColorStop {
    float position;   // 0..1
    float value;      // output value at this position
};

class ColorRampNode : public Node {
public:
    ColorRampNode() { m_stops.push_back({0.0f, 0.0f}); m_stops.push_back({1.0f, 1.0f}); }

    void addStop(float pos, float val) {
        m_stops.push_back({pos, val});
        std::sort(m_stops.begin(), m_stops.end(),
                  [](const ColorStop& a, const ColorStop& b) { return a.position < b.position; });
    }

    void setStops(const std::vector<ColorStop>& stops) { m_stops = stops; }

    std::string name() const override { return "Color Ramp"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"Factor", DataType::Float}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Result", DataType::Float}};
    }
    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float, float, float) override {
        float t = std::clamp(inputs[0].asFloat(), 0.0f, 1.0f);
        if (m_stops.empty()) { outputs[0] = Value::Float(t); return; }
        if (t <= m_stops.front().position) { outputs[0] = Value::Float(m_stops.front().value); return; }
        if (t >= m_stops.back().position)  { outputs[0] = Value::Float(m_stops.back().value); return; }
        for (size_t i = 1; i < m_stops.size(); i++) {
            if (t < m_stops[i].position) {
                float p = (t - m_stops[i-1].position) / (m_stops[i].position - m_stops[i-1].position);
                outputs[0] = Value::Float(m_stops[i-1].value + (m_stops[i].value - m_stops[i-1].value) * p);
                return;
            }
        }
        outputs[0] = Value::Float(m_stops.back().value);
    }

private:
    std::vector<ColorStop> m_stops;
};

// ===========================================================================
// Clamp node
// ===========================================================================
class ClampNode : public Node {
public:
    ClampNode(float min = 0, float max = 1) : m_min(min), m_max(max) {}

    std::string name() const override { return "Clamp"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"Value", DataType::Float},
                {"Min", DataType::Float, Value::Float(m_min)},
                {"Max", DataType::Float, Value::Float(m_max)}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Result", DataType::Float}};
    }
    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float, float, float) override {
        outputs[0] = Value::Float(std::clamp(inputs[0].asFloat(),
                                             inputs[1].asFloat(),
                                             inputs[2].asFloat()));
    }

private:
    float m_min, m_max;
};

// ===========================================================================
// Map range — remap value from input range to output range
// ===========================================================================
class MapRangeNode : public Node {
public:
    MapRangeNode(float inMin = 0, float inMax = 1, float outMin = 0, float outMax = 1)
        : m_inMin(inMin), m_inMax(inMax), m_outMin(outMin), m_outMax(outMax) {}

    std::string name() const override { return "Map Range"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"Value", DataType::Float},
                {"In Min", DataType::Float, Value::Float(m_inMin)},
                {"In Max", DataType::Float, Value::Float(m_inMax)},
                {"Out Min", DataType::Float, Value::Float(m_outMin)},
                {"Out Max", DataType::Float, Value::Float(m_outMax)}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Result", DataType::Float}};
    }
    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float, float, float) override {
        float v = inputs[0].asFloat();
        float inMin = inputs[1].asFloat();
        float inMax = inputs[2].asFloat();
        float outMin = inputs[3].asFloat();
        float outMax = inputs[4].asFloat();
        float range = inMax - inMin;
        float t = range != 0 ? (v - inMin) / range : 0;
        outputs[0] = Value::Float(outMin + t * (outMax - outMin));
    }

private:
    float m_inMin, m_inMax, m_outMin, m_outMax;
};

// ===========================================================================
// Domain node — transforms the sample coordinate
// ===========================================================================
class DomainNode : public Node {
public:
    enum Op { Scale, Offset, Tile, Mirror, Warp };

    DomainNode(Op op = Op::Scale, float amount = 1.0f)
        : m_op(op), m_amount(amount) {}

    std::string name() const override {
        switch (m_op) {
            case Op::Scale:  return "Scale";
            case Op::Offset: return "Offset";
            case Op::Tile:   return "Tile";
            case Op::Mirror: return "Mirror";
            case Op::Warp:   return "Warp";
        }
        return "Domain";
    }

    // The first input is the node to sample, the rest are domain parameters.
    // We override evaluate to modify coords before passing to input[0].
    std::vector<SocketDesc> inputs() const override {
        switch (m_op) {
            case Op::Scale:  return {{"Source", DataType::Float},
                                     {"Scale", DataType::Float, Value::Float(m_amount)}};
            case Op::Offset: return {{"Source", DataType::Float},
                                     {"Offset", DataType::Float, Value::Float(m_amount)}};
            case Op::Tile:   return {{"Source", DataType::Float},
                                     {"Size", DataType::Float, Value::Float(m_amount)}};
            case Op::Mirror: return {{"Source", DataType::Float},
                                     {"Size", DataType::Float, Value::Float(m_amount)}};
            case Op::Warp:   return {{"Source", DataType::Float},
                                     {"Amount", DataType::Float, Value::Float(m_amount)},
                                     {"Noise", DataType::Float}}; // warping noise connected here
        }
        return {};
    }

    std::vector<SocketDesc> outputs() const override {
        return {{"Result", DataType::Float}};
    }

    void evaluate(const Value* inputs, int numIn, Value* outputs, int,
                  float x, float y, float z) override;

private:
    Op m_op;
    float m_amount;
};

inline void DomainNode::evaluate(const Value* inputs, int, Value* outputs, int,
                                  float x, float y, float z) {
    float amount = inputs[1].asFloat();
    float nx = x, ny = y, nz = z;
    switch (m_op) {
        case Op::Scale:
            nx = x * amount; ny = y * amount; nz = z * amount;
            break;
        case Op::Offset:
            nx = x + amount; ny = y + amount; nz = z + amount;
            break;
        case Op::Tile: {
            float s = std::max(amount, 0.001f);
            nx = std::fmod(x, s); if (nx < 0) nx += s;
            ny = std::fmod(y, s); if (ny < 0) ny += s;
            nz = std::fmod(z, s); if (nz < 0) nz += s;
            break;
        }
        case Op::Mirror: {
            float s = std::max(amount, 0.001f);
            auto mirror = [s](float v) {
                float t = std::fmod(v, 2 * s); if (t < 0) t += 2 * s;
                return t < s ? t : 2 * s - t;
            };
            nx = mirror(x); ny = mirror(y); nz = mirror(z);
            break;
        }
        case Op::Warp: {
            float warp = inputs[2].asFloat() * amount;
            nx = x + warp; ny = y + warp; nz = z + warp;
            break;
        }
    }

    // Re-evaluate the source subgraph at the transformed coordinates
    if (m_graph) {
        outputs[0] = m_graph->evaluateInputAt(m_nodeIndex, 0, nx, ny, nz);
    } else {
        outputs[0] = inputs[0];
    }
}

// ===========================================================================
// Gradient node — linear, radial, or spherical gradient
// ===========================================================================
class GradientNode : public Node {
public:
    enum Type { LinearX, LinearY, LinearZ, Radial, Spherical };

    GradientNode(Type type = LinearX) : m_type(type) {}

    std::string name() const override {
        switch (m_type) {
            case LinearX:    return "Gradient X";
            case LinearY:    return "Gradient Y";
            case LinearZ:    return "Gradient Z";
            case Radial:     return "Radial Gradient";
            case Spherical:  return "Spherical Gradient";
        }
        return "Gradient";
    }

    std::vector<SocketDesc> inputs() const override {
        return {{"Scale", DataType::Float, Value::Float(1.0f)},
                {"Offset", DataType::Float, Value::Float(0.0f)}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Value", DataType::Float}};
    }

    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float x, float y, float z) override {
        float scale = inputs[0].asFloat();
        float off = inputs[1].asFloat();
        float v = 0;
        switch (m_type) {
            case LinearX:    v = x; break;
            case LinearY:    v = y; break;
            case LinearZ:    v = z; break;
            case Radial:     v = std::sqrt(x*x + y*y); break;
            case Spherical:  v = std::sqrt(x*x + y*y + z*z); break;
        }
        outputs[0] = Value::Float(v / scale + off);
    }

private:
    Type m_type;
};

// ===========================================================================
// Checker node — standard checkerboard pattern
// ===========================================================================
class CheckerNode : public Node {
public:
    CheckerNode(float size = 1.0f) : m_size(size) {}

    std::string name() const override { return "Checker"; }
    std::vector<SocketDesc> inputs() const override {
        return {{"Size", DataType::Float, Value::Float(m_size)}};
    }
    std::vector<SocketDesc> outputs() const override {
        return {{"Value", DataType::Float}};
    }

    void evaluate(const Value* inputs, int, Value* outputs, int,
                  float x, float y, float z) override {
        float s = std::max(inputs[0].asFloat(), 0.001f);
        int cx = (int)std::floor(x / s);
        int cy = (int)std::floor(y / s);
        int cz = (int)std::floor(z / s);
        bool on = ((cx + cy + cz) & 1) != 0;
        outputs[0] = Value::Float(on ? 1.0f : 0.0f);
    }

private:
    float m_size;
};

}}} // namespace Haruka::Tools::ProcGraph