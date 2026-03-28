#pragma once

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace gladius::compute
{
    /// Termination criterion for fast QEM simplification
    enum class SimplificationTerminationMode
    {
        TargetTriangleCount,    ///< Stop at specified triangle count
        TargetReductionPercent, ///< Stop at specified reduction percentage
        ErrorBounded            ///< Stop when all remaining collapses exceed error threshold
    };

    /// Configuration for fast geometric QEM simplification (CPU-only, single-pass)
    struct FastQemConfig
    {
        /// Termination mode
        SimplificationTerminationMode terminationMode{SimplificationTerminationMode::TargetReductionPercent};

        /// Target triangle count (used when terminationMode == TargetTriangleCount)
        std::size_t targetTriangleCount{0U};

        /// Target reduction percentage 0-100 (used when terminationMode == TargetReductionPercent)
        float targetReductionPercent{50.0F};

        /// Maximum error for edge collapse (used when terminationMode == ErrorBounded,
        /// also as a hard cap in other modes). In squared world units.
        float maxError{std::numeric_limits<float>::max()};

        /// Threshold for flip detection: minimum dot product between old and new normals
        float flipThreshold{0.2F};

        /// How often to check for cancellation (every N collapses)
        std::uint32_t cancelCheckPeriod{16U};
    };

    /// Double-precision 4x4 symmetric matrix for Quadric Error Metrics.
    /// Stores 10 unique elements of the upper triangle:
    /// [m0  m1  m2  m3]
    /// [    m4  m5  m6]
    /// [        m7  m8]
    /// [            m9]
    struct SymMat
    {
        double m[10]{};

        SymMat() = default;

        /// Construct from all 10 elements
        SymMat(double m0, double m1, double m2, double m3,
               double m4, double m5, double m6,
               double m7, double m8,
               double m9)
            : m{m0, m1, m2, m3, m4, m5, m6, m7, m8, m9}
        {
        }

        /// Construct from plane equation ax + by + cz + d = 0 as outer product p*p^T
        static SymMat fromPlane(double a, double b, double c, double d)
        {
            return {a * a, a * b, a * c, a * d,
                           b * b, b * c, b * d,
                                  c * c, c * d,
                                         d * d};
        }

        SymMat & operator+=(SymMat const & rhs)
        {
            for (int i = 0; i < 10; ++i)
            {
                m[i] += rhs.m[i];
            }
            return *this;
        }

        SymMat operator+(SymMat const & rhs) const
        {
            auto result = *this;
            result += rhs;
            return result;
        }

        /// Determinant of a 3x3 sub-matrix formed by selecting rows/cols from the 4x4 matrix.
        /// Row i of the 4x4 matrix is indexed as: (r0,c0), (r0,c1), (r0,c2); etc.
        /// Indices reference the flat m[10] array via the index accessor.
        [[nodiscard]] double det(int a11, int a12, int a13,
                                 int a21, int a22, int a23,
                                 int a31, int a32, int a33) const
        {
            return m[a11] * (m[a22] * m[a33] - m[a23] * m[a32])
                 - m[a12] * (m[a21] * m[a33] - m[a23] * m[a31])
                 + m[a13] * (m[a21] * m[a32] - m[a22] * m[a31]);
        }

        /// Evaluate quadric error at point v: v^T * Q * v (homogeneous: w=1)
        [[nodiscard]] double evaluate(Eigen::Vector3f const & v) const
        {
            double const x = v.x();
            double const y = v.y();
            double const z = v.z();
            return x * x * m[0] + 2.0 * x * y * m[1] + 2.0 * x * z * m[2] + 2.0 * x * m[3]
                 + y * y * m[4] + 2.0 * y * z * m[5] + 2.0 * y * m[6]
                 + z * z * m[7] + 2.0 * z * m[8]
                 + m[9];
        }

        /// Find optimal vertex position that minimizes quadric error.
        /// Returns nullopt if the 3x3 sub-matrix is singular.
        [[nodiscard]] std::optional<Eigen::Vector3f> optimalVertex() const
        {
            // Solve: A * v = -b  where A = top-left 3x3, b = top-right 3x1
            double const d = det(0, 1, 2, 1, 4, 5, 2, 5, 7);
            if (std::abs(d) < 1e-15)
            {
                return std::nullopt;
            }
            double const invD = 1.0 / d;
            double const x = -invD * det(3, 1, 2, 6, 4, 5, 8, 5, 7);
            double const y = -invD * det(0, 3, 2, 1, 6, 5, 2, 8, 7);
            double const z = -invD * det(0, 1, 3, 1, 4, 6, 2, 5, 8);
            return Eigen::Vector3f(static_cast<float>(x),
                                   static_cast<float>(y),
                                   static_cast<float>(z));
        }
    };

    /// Mutable min-heap with O(log n) push/pop/update/remove.
    /// IndexSetter is called as indexSetter(element&, heap_index) to let elements track their position.
    /// LessPredicate compares two elements; returns true if the first is "less" (higher priority).
    template<typename T, typename IndexSetter, typename LessPredicate>
    class MutablePriorityQueue
    {
      public:
        MutablePriorityQueue(IndexSetter indexSetter, LessPredicate less)
            : m_indexSetter(std::move(indexSetter))
            , m_less(std::move(less))
        {
        }

        void reserve(std::size_t count) { m_heap.reserve(count); }
        void clear() { m_heap.clear(); }
        [[nodiscard]] std::size_t size() const { return m_heap.size(); }
        [[nodiscard]] bool empty() const { return m_heap.empty(); }
        T & top() { return m_heap.front(); }
        T const & top() const { return m_heap.front(); }
        T & operator[](std::size_t heapIndex) { return m_heap[heapIndex]; }

        void push(T const & item)
        {
            m_heap.push_back(item);
            m_indexSetter(m_heap.back(), m_heap.size() - 1U);
            bubbleUp(m_heap.size() - 1U);
        }

        void pop()
        {
            if (m_heap.size() > 1U)
            {
                swapElements(0U, m_heap.size() - 1U);
                m_heap.pop_back();
                if (!m_heap.empty())
                {
                    bubbleDown(0U);
                }
            }
            else
            {
                m_heap.pop_back();
            }
        }

        void remove(std::size_t heapIndex)
        {
            if (heapIndex >= m_heap.size())
            {
                return;
            }
            if (heapIndex == m_heap.size() - 1U)
            {
                m_heap.pop_back();
                return;
            }
            swapElements(heapIndex, m_heap.size() - 1U);
            m_heap.pop_back();
            if (heapIndex < m_heap.size())
            {
                bubbleUp(heapIndex);
                bubbleDown(heapIndex);
            }
        }

        void update(std::size_t heapIndex)
        {
            if (heapIndex >= m_heap.size())
            {
                return;
            }
            bubbleUp(heapIndex);
            bubbleDown(heapIndex);
        }

      private:
        std::vector<T> m_heap;
        IndexSetter m_indexSetter;
        LessPredicate m_less;

        void swapElements(std::size_t a, std::size_t b)
        {
            std::swap(m_heap[a], m_heap[b]);
            m_indexSetter(m_heap[a], a);
            m_indexSetter(m_heap[b], b);
        }

        void bubbleUp(std::size_t idx)
        {
            while (idx > 0U)
            {
                std::size_t const parent = (idx - 1U) / 2U;
                if (m_less(m_heap[idx], m_heap[parent]))
                {
                    swapElements(idx, parent);
                    idx = parent;
                }
                else
                {
                    break;
                }
            }
        }

        void bubbleDown(std::size_t idx)
        {
            std::size_t const sz = m_heap.size();
            for (;;)
            {
                std::size_t const left = 2U * idx + 1U;
                std::size_t const right = 2U * idx + 2U;
                std::size_t smallest = idx;
                if (left < sz && m_less(m_heap[left], m_heap[smallest]))
                {
                    smallest = left;
                }
                if (right < sz && m_less(m_heap[right], m_heap[smallest]))
                {
                    smallest = right;
                }
                if (smallest == idx)
                {
                    break;
                }
                swapElements(idx, smallest);
                idx = smallest;
            }
        }
    };

    /// Simplify an indexed triangle set using fast greedy QEM.
    ///
    /// @param positions  IN/OUT vertex positions
    /// @param indices    IN/OUT triangle indices (3 per triangle)
    /// @param config     Simplification parameters
    /// @param throwOnCancel  Called periodically; throw to cancel
    /// @param progressFn     Called with progress 0-100
    /// @return Number of edges collapsed
    std::size_t fastQemSimplify(
        std::vector<Eigen::Vector3f> & positions,
        std::vector<std::uint32_t> & indices,
        FastQemConfig const & config,
        std::function<void()> throwOnCancel = nullptr,
        std::function<void(int)> progressFn = nullptr);

} // namespace gladius::compute
