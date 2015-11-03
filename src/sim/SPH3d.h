#pragma once


// [1] Weakly compressible SPH for free surface flows
// [2] Predictive-Corrective Incompressible SPH
// [3] Versatile Surface Tension and Adhesion for SPH Fluids

#include "core/Common.h"
#include "core/Vector.h"
#include "core/Box.h"
#include "core/Morton.h"
#include "core/AlignedAllocator.h"
#include "core/Timer.h"
#include "core/Profiler.h"
#include "sim/Scene.h"

#include <tbb/tbb.h>

#include <vector>

#define USE_TBB 1

namespace pbs {
namespace sph3d {

class Grid {
public:
    void init(const Box3f &bounds, float cellSize) {
        _bounds = bounds;
        _cellSize = cellSize;
        _invCellSize = 1.f / cellSize;

        _size = Vector3i(
            nextPowerOfTwo(int(std::floor(_bounds.extents().x() / _cellSize)) + 1),
            nextPowerOfTwo(int(std::floor(_bounds.extents().y() / _cellSize)) + 1),
            nextPowerOfTwo(int(std::floor(_bounds.extents().z() / _cellSize)) + 1)
        );

        _cellOffset.resize(_size.prod() + 1);

        DBG("Initialized grid: bounds = %s, cellSize = %f, size = %s", _bounds, _cellSize, _size);
    }

    inline Vector3i index(const Vector3f &pos) {
        return Vector3i(
            int(std::floor((pos.x() - _bounds.min.x()) * _invCellSize)),
            int(std::floor((pos.y() - _bounds.min.y()) * _invCellSize)),
            int(std::floor((pos.z() - _bounds.min.z()) * _invCellSize))
        );
    }

    inline size_t indexLinear(const Vector3f &pos) {
        Vector3i i = index(pos);
        return i.z() * (_size.x() * _size.y()) + i.y() * _size.x() + i.x();
    }

    inline uint32_t indexMorton(const Vector3i &index) {
        return Morton3D::morton10bit(index.x(), index.y(), index.z());
    }

    inline uint32_t indexMorton(const Vector3f &pos) {
        return indexMorton(index(pos));
    }

    template<typename SwapFunc>
    void update(const std::vector<Vector3f> &positions, SwapFunc swap) {
        std::vector<uint32_t> cellCount(_size.prod(), 0);
        std::vector<uint32_t> cellIndex(_size.prod(), 0);

        size_t count = positions.size();

        std::vector<uint32_t> indices(count);

        // Update particle index and count number of particles per cell
        for (size_t i = 0; i < count; ++i) {
            uint32_t index = indexLinear(positions[i]);
            //uint32_t index = indexMorton(particles[i].p);
            indices[i] = index;
            ++cellCount[index];
        }

        // Initialize cell indices & offsets
        size_t index = 0;
        for (size_t i = 0; i < cellIndex.size(); ++i) {
            cellIndex[i] = index;
            _cellOffset[i] = index;
            index += cellCount[i];
        }
        _cellOffset.back() = index;

        // Sort particles by index
        for (size_t i = 0; i < count; ++i) {
            while (i >= cellIndex[indices[i]] || i < _cellOffset[indices[i]]) {
                size_t j = cellIndex[indices[i]]++;
                std::swap(indices[i], indices[j]);
                swap(i, j);
            }
        }
    }

    template<typename Func>
    void lookup(const Vector3f &pos, float radius, Func func) {
        Vector3i min = index(pos - Vector3f(radius)).cwiseMax(Vector3i(0));
        Vector3i max = index(pos + Vector3f(radius)).cwiseMin(_size - Vector3i(1));
        for (int z = min.z(); z <= max.z(); ++z) {
            for (int y = min.y(); y <= max.y(); ++y) {
                for (int x = min.x(); x <= max.x(); ++x) {
                    size_t i = z * (_size.x() * _size.y()) + y * _size.x() + x;
                    for (size_t j = _cellOffset[i]; j < _cellOffset[i + 1]; ++j) {
                        func(j);
                    }
                }
            }
        }
    }

private:
    Box3f _bounds;
    float _cellSize;
    float _invCellSize;

    Vector3i _size;
    std::vector<size_t> _cellOffset;
};

class SPH {
public:
    // Simulation settings
    struct Settings {
        // Stiffness constant
        float stiffness = 3.f;
        // Viscosity
        float viscosity = 1.f;
        // Gravity force
        Vector3f gravity = Vector3f(0.f, -9.81f, 0.f);
    };

    // Simulation parameters
    struct Parameters {
        int supportParticles;
        int particlesPerUnitVolume;
        float restDensity;
        float restSpacing;
        float particleMass;
        float h;
    };

    // Kernels
    struct Kernel {
        float h;
        float h2;
        float halfh;

        void init(float h_) {
            h = h_;
            h2 = sqr(h);
            halfh = 0.5f * h;
            poly6Constant = 365.f / (64.f * M_PI * std::pow(h, 9.f));
            poly6GradConstant = -945.f / (32.f * M_PI * std::pow(h, 9.f));
            poly6LaplaceConstant = -945.f / (32.f * M_PI * std::pow(h, 9.f));
            spikyConstant = 15.f / (M_PI * std::pow(h, 6.f));
            spikyGradConstant = -45.f / (M_PI * std::pow(h, 6.f));
            spikyLaplaceConstant = -90.f / (M_PI * std::pow(h, 6.f));
            viscosityLaplaceConstant = 45.f / (M_PI * std::pow(h, 6.f));

            surfaceTensionConstant = 32.f / (M_PI * std::pow(h, 9.f));
            surfaceTensionOffset = -std::pow(h, 6.f) / 64.f;
        }

        // Kernels are split into constant and variable part. Arguments are as follows:
        // r  = displacement vector
        // r2 = |r|^2 (squared norm of r)
        // rn = |r|   (norm of r)

        float poly6Constant;
        inline float poly6(float r2) const {
            return cube(h2 - r2);
        }

        float poly6GradConstant;
        inline Vector3f poly6Grad(const Vector3f &r, float r2) const {
            return sqr(h2 - r2) * r;
        }

        float poly6LaplaceConstant;
        inline float poly6Laplace(float r2) {
            return (h2 - r2) * (3.f * h2 - 7.f * r2);
        }

        float spikyConstant;
        inline float spiky(float rn) const {
            return cube(h - rn);
        }

        float spikyGradConstant;
        inline Vector3f spikyGrad(const Vector3f &r, float rn) const {
            return sqr(h - rn) * r * (1.f / rn);
        }

        float spikyLaplaceConstant;
        inline float spikyLaplace(float rn) const {
            return (h - rn) * (h - 2.f * rn) / rn;
        }

        float viscosityLaplaceConstant;
        inline float viscosityLaplace(float rn) const {
            return (h - rn);
        }

        float surfaceTensionConstant;
        float surfaceTensionOffset;
        inline float surfaceTension(float rn) const {
            if (rn < halfh) {
                return 2.f * cube(h - rn) * cube(rn) + surfaceTensionOffset;
            } else {
                return cube(h - rn) * cube(rn);
            }
        }
    };

    SPH(const Scene &scene) {
        _supportParticles = scene.settings.getInteger("supportParticles", _supportParticles);
        _particlesPerUnitVolume = scene.settings.getInteger("particlesPerUnitVolume", _particlesPerUnitVolume);
        _restDensity = scene.settings.getFloat("restDensity", _restDensity);

        _restSpacing = 1.f / std::pow(_particlesPerUnitVolume, 1.f / 3.f);
        _particleMass = _restDensity / _particlesPerUnitVolume;
        _particleMass2 = sqr(_particleMass);
        //_h = std::pow((3.f * _supportParticles) / (4.f * M_PI * _particlesPerUnitVolume), 1.f / 3.f);
        _h = _restSpacing * 2.f;
        _h2 = sqr(_h);

        wcsph.B = _restDensity * sqr(wcsph.cs) / wcsph.gamma;
        wcsph.dt = std::min(0.25f * _h / (_particleMass * 9.81f), 0.4f * _h / (wcsph.cs * (1.f + 0.6f * wcsph.viscosity)));

        _maxTimestep = 1e-3f;

        _bounds = scene.world.bounds;
        _kernel.init(_h);
        _grid.init(_bounds, _h);

        DBG("supportParticles = %d", _supportParticles);
        DBG("particlesPerUnitVolume = %d", _particlesPerUnitVolume);
        DBG("restDensity = %f", _restDensity);
        DBG("settings.stiffness = %f", _settings.stiffness);
        DBG("settings.viscosity = %f", _settings.viscosity);
        DBG("restSpacing = %f", _restSpacing);
        DBG("particleMass = %f", _particleMass);
        DBG("h = %f", _h);
        DBG("wcsph.gamma = %f", wcsph.gamma);
        DBG("wcsph.cs = %f", wcsph.cs);
        DBG("wcsph.B = %f", wcsph.B);
        DBG("wcsph.viscosity = %f", wcsph.viscosity);
        DBG("wcsph.dt = %f", wcsph.dt);

        for (const auto box : scene.boxes) {
            voxelizeBox(box.bounds);
        }
        for (const auto sphere : scene.spheres) {
            voxelizeSphere(sphere.position, sphere.radius);
        }

        //voxelizeBox(Box3f(Vector3f(0.25f), Vector3f(0.75f)));
        //voxelizeBox(Box3f(Vector3f(0.1f, 0.5f), Vector3f(0.9f, 0.9f)));
        //voxelizeBox(Box3f(Vector3f(0.3f, 0.5f), Vector3f(0.7f, 0.9f)));
        //voxelizeBox(Box3f(Vector3f(0.4f), Vector3f(0.6f)));


        DBG("simulating %d particles ...", _positions.size());

        _velocities.resize(_positions.size());
        _normals.resize(_positions.size());
        _forces.resize(_positions.size());
        _densities.resize(_positions.size());
        _pressures.resize(_positions.size());
        _indices.resize(_positions.size());

    }

    const Settings &settings() const { return _settings; }
          Settings &settings()       { return _settings; }

    // iterate i=0..count-1 calling func(i)
    template<typename Func>
    void iterate(size_t count, Func func) {
#if USE_TBB
        tbb::parallel_for(0ul, count, 1ul, [func] (size_t i) { func(i); });
#else
        for (size_t i = 0; i < count; ++i) { func(i); }
#endif
    }

    // iterate over all neighbours around p, calling func(j, r, r2)
    template<typename Func>
    void iterateNeighbours(const Vector3f &p, Func func) {
        _grid.lookup(p, _h, [this, func, &p] (size_t j) {
            Vector3f r = p - _positions[j];
            float r2 = r.squaredNorm();
            if (r2 < _h2) {
                func(j, r, r2);
            }
        });
    }

    void computeDensity() {
        iterate(_positions.size(), [this] (size_t i) {
            float density = 0.f;
            iterateNeighbours(_positions[i], [this, &density] (size_t j, const Vector3f &r, float r2) {
                density += _kernel.poly6(r2);
            });
            density *= _particleMass * _kernel.poly6Constant;

            // Tait pressure (WCSPH)
            float t = density / _restDensity;
            float pressure = wcsph.B * ((t*t)*(t*t)*(t*t)*t - 1.f);

            _densities[i] = density;
            _pressures[i] = pressure;
        });
    }

    // Compute normals based on [3]
    void computeNormals() {
        iterate(_positions.size(), [this] (size_t i) {
            Vector3f normal;
            iterateNeighbours(_positions[i], [this, &normal] (size_t j, const Vector3f &r, float r2) {
                normal += _kernel.poly6Grad(r, r2) / _densities[j];
            });
            normal *= _h * _particleMass * _kernel.poly6GradConstant;
            _normals[i] = normal;
        });
    }

    void computeForces() {
        iterate(_positions.size(), [this] (size_t i) {
            Vector3f force(0.f);
            Vector3f forceViscosity;
            Vector3f forceCohesion;
            Vector3f forceCurvature;

            _grid.lookup(_positions[i], _h, [this, i, &force, &forceCohesion, &forceCurvature, &forceViscosity] (size_t j) {
                const Vector3f &v_i = _velocities[i];
                const Vector3f &v_j = _velocities[j];
                const Vector3f &n_i = _normals[i];
                const Vector3f &n_j = _normals[j];
                const float &density_i = _densities[i];
                const float &density_j = _densities[j];
                const float &pressure_i = _pressures[i];
                const float &pressure_j = _pressures[j];

                if (i != j) {
                    Vector3f r = _positions[i] - _positions[j];
                    float r2 = r.squaredNorm();
                    if (r2 < _h2 && r2 > 0.00001f) {
                        float rn = std::sqrt(r2);
                        //force -= 0.5f * (pressure_i + pressure_j) * _m / density_j * Kernel::spikyGrad(r);
                        //force -= _particleMass2 * (pressure_i + pressure_j) / (2.f * density_i * density_j) * _kernel.spikyGradConstant * _kernel.spikyGrad(r, rn);

                        // Viscosity force
                        //force += _particleMass2 * _settings.viscosity * (v_j - v_i) / (density_i * density_j) * _kernel.viscosityLaplaceConstant * _kernel.viscosityLaplace(rn);


                        // Pressure force (WCSPH)
                        //if (pressure_i > 0.f || pressure_j > 0.f)
                        force -= _particleMass2 * (pressure_i / sqr(density_i) + pressure_j / sqr(density_j)) * _kernel.spikyGradConstant * _kernel.spikyGrad(r, rn);

                        #if 0
                        // Viscosity force (WCSPH)
                        Vector3f v = (v_i - v_j);
                        if (v.dot(r) < 0.f) {
                            float vu = 2.f * wcsph.viscosity * _h * wcsph.cs / (density_i + density_j);
                            force += vu * _particleMass2 * (v.dot(r) / (r2 + 0.001f * sqr(_h))) * _kernel.spikyGradConstant * _kernel.spikyGrad(r, rn);
                        }
                        #endif

                        // Surface tension force (WCSPH)
                        #if 0
                        float K = 0.1f;
                        Vector3f a = -K * _kernel.poly6Constant * _kernel.poly6(r2) * r / rn;
                        force += _particleMass * a;
                        #endif

                        // Viscosity
                        if (density_j > 0.0001f) {
                            forceViscosity -= (v_i - v_j) * (_kernel.viscosityLaplace(rn) / density_j);
                        }

                        // Surface tension (according to [3])
                        float correctionFactor = 2.f * _restDensity / (density_i + density_j);
                        forceCohesion += correctionFactor * r * _kernel.surfaceTension(rn);
                        forceCurvature += correctionFactor * (n_i - n_j);
                    } else if (r2 == 0.f) {
                        // Avoid collapsing particles
                        _positions[j] += Vector3f(1e-5f);
                    }
                }
            });

            const float viscosity = 0.0001f;
            forceViscosity *= viscosity * _particleMass * _kernel.viscosityLaplaceConstant;

            const float surfaceTension = 1.f;
            forceCohesion *= surfaceTension * _particleMass2 * _kernel.surfaceTensionConstant;
            forceCurvature *= surfaceTension * _particleMass;

            force += forceCohesion + forceCurvature + forceViscosity;
            force += _particleMass * _settings.gravity;

            _forces[i] = force;
        });
    }

    void computeCollisions(std::function<void(size_t i, const Vector3f &n, float d)> handler) {
        for (size_t i = 0; i < _positions.size() - 1; ++i) {
            const auto &p = _positions[i];
            if (p.x() < _bounds.min.x()) {
                handler(i, Vector3f(1.f, 0.f, 0.f), _bounds.min.x() - p.x());
            }
            if (p.x() > _bounds.max.x()) {
                handler(i, Vector3f(-1.f, 0.f, 0.f), p.x() - _bounds.max.x());
            }
            if (p.y() < _bounds.min.y()) {
                handler(i, Vector3f(0.f, 1.f, 0.f), _bounds.min.y() - p.y());
            }
            if (p.y() > _bounds.max.y()) {
                handler(i, Vector3f(0.f, -1.f, 0.f), p.y() - _bounds.max.y());
            }
            if (p.z() < _bounds.min.z()) {
                handler(i, Vector3f(0.f, 0.f, 1.f), _bounds.min.z() - p.z());
            }
            if (p.z() > _bounds.max.z()) {
                handler(i, Vector3f(0.f, 0.f, -1.f), p.z() - _bounds.max.z());
            }
        }
    }

    void update(float dt) {
        _t += dt;

        DBG("update: dt = %f", dt);

        Vector3f gd;
        float t = std::fmod(_t * 0.5f, 4.f);
        if (t < 1.f) {
            _settings.gravity = Vector3f(0.f, -9.81f, 0.f);
        } else if (t < 2.f) {
            _settings.gravity = Vector3f(9.81f, 0.f, 0.f);
        } else if (t < 3.f) {
            _settings.gravity = Vector3f(0.f, 9.81f, 0.f);
        } else {
            _settings.gravity = Vector3f(-9.81f, 0.f, 0.f);
        }

        //_settings.gravity = Vector3f(0.f);
        _settings.gravity = Vector3f(0.f, -9.81f, 0.f);

        {
            ProfileScope profile("Grid Update");
            _grid.update(_positions, [this] (size_t i, size_t j) {
                std::swap(_positions[i], _positions[j]);
                std::swap(_velocities[i], _velocities[j]);
            });
        }

        {
            ProfileScope profile("Density Update");
            computeDensity();
        }

        {
            ProfileScope profile("Normal Update");
            computeNormals();
        }

        {
            ProfileScope profile("Force Update");
            computeForces();
        }

        {
            ProfileScope profile("Integrate");
            float invM = 1.f / _particleMass;
            iterate(_positions.size(), [this, invM, dt] (size_t i) {
                Vector3f a = _forces[i] * invM;
                _velocities[i] += a * dt;
                _positions[i] += _velocities[i] * dt;
            });
        }

        {
            ProfileScope profile("Collision Update");

            // Collision handling
            computeCollisions([this] (size_t i, const Vector3f &n, float d) {
                float c = 0.5f;
                _positions[i] += n * d;
                _velocities[i] -= (1 + c) * _velocities[i].dot(n) * n;
            });
        }

        Profiler::dump();
    }


    void voxelizeBox(const Box3f &box) {
        Vector3i min(
            int(std::ceil(box.min.x() / _restSpacing)),
            int(std::ceil(box.min.y() / _restSpacing)),
            int(std::ceil(box.min.z() / _restSpacing))
        );
        Vector3i max(
            int(std::floor(box.max.x() / _restSpacing)),
            int(std::floor(box.max.y() / _restSpacing)),
            int(std::floor(box.max.z() / _restSpacing))
        );
        for (int z = min.z(); z <= max.z(); ++z) {
            for (int y = min.y(); y <= max.y(); ++y) {
                for (int x = min.x(); x <= max.x(); ++x) {
                    Vector3f p(x * _restSpacing, y * _restSpacing, z * _restSpacing);
                    _positions.emplace_back(p);
                }
            }
        }
    }

    void voxelizeSphere(const Vector3f &pos, float radius) {
        Vector3i min(
            int(std::ceil((pos.x() - radius) / _restSpacing)),
            int(std::ceil((pos.y() - radius) / _restSpacing)),
            int(std::ceil((pos.z() - radius) / _restSpacing))
        );
        Vector3i max(
            int(std::floor((pos.x() + radius) / _restSpacing)),
            int(std::floor((pos.y() + radius) / _restSpacing)),
            int(std::floor((pos.z() + radius) / _restSpacing))
        );
        float r2 = sqr(radius);
        for (int z = min.z(); z <= max.z(); ++z) {
            for (int y = min.y(); y <= max.y(); ++y) {
                for (int x = min.x(); x <= max.x(); ++x) {
                    Vector3f p(x * _restSpacing, y * _restSpacing, z * _restSpacing);
                    if ((p - pos).squaredNorm() <= r2) {
                        _positions.emplace_back(p);
                    }
                }
            }
        }
    }

    const Box3f &bounds() const { return _bounds; }
    //const ParticleVector &particles() const { return _particles; }

    // Returns a set of simulation parameters
    Parameters parameters() const {
        Parameters params;
        params.supportParticles = _supportParticles;
        params.particlesPerUnitVolume = _particlesPerUnitVolume;
        params.restDensity = _restDensity;
        params.restSpacing = _restSpacing;
        params.particleMass = _particleMass;
        params.h = _h;
        return params;
    }

    float maxTimestep() const { return _maxTimestep; }

    // Returns particle positions in matrix form
    MatrixXf positions() const {
        MatrixXf positions;
        positions.resize(3, _positions.size());
        for (size_t i = 0; i < _positions.size(); ++i) {
            positions.col(i) = _positions[i];
        }
        return std::move(positions);
    }

private:
    int _supportParticles = 50;             ///< Number of particles expected to be within smoothing kernel support
    int _particlesPerUnitVolume = 1000000;  ///< Number of particles per unit volume
    float _restDensity = 1000.f;            ///< Rest density in kg/m^3

    float _restSpacing;                     ///< Particle grid spacing on initialization
    float _particleMass;                    ///< Particle mass
    float _particleMass2;                   ///< Squared particle mass
    float _h;                               ///< SPH smoothing radius
    float _h2;                              ///< Squared SPH smooting radius

    float _maxTimestep;                     ///< Maximum allowed timestep

    struct {
        const float gamma = 7.f;
        float cs = 10.f;
        float B;
        float viscosity = 0.005f;
        float dt;
    } wcsph;

    Settings _settings;

    Kernel _kernel;

    Box3f _bounds;
    Grid _grid;

    //ParticleVector _particles;

    // Fluid particle buffers
    std::vector<Vector3f> _positions;
    std::vector<Vector3f> _velocities;
    std::vector<Vector3f> _normals;
    std::vector<Vector3f> _forces;
    std::vector<float> _densities;
    std::vector<float> _pressures;
    std::vector<uint32_t> _indices;


    float _t = 0.f;

};

} // namespace sph3d
} // namespace pbs