#include "IExporter.h"
#include "CancellationToken.h"

namespace gladius::io
{
    bool IExporter::isCancellationRequested() const
    {
        if (m_cancellationToken == nullptr)
        {
            return false;
        }
        return m_cancellationToken->isCancelled();
    }
}
