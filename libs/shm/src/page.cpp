#include "page_impl.h"

#include <dztrader/shm/page.h>

namespace dztrader::shm {

Page::Page(const ImplPtr& impl) : address_(impl->address()), page_id_(impl->page_id()), size_(impl->size()), impl_(impl)
{
}

}  // namespace dztrader::shm
