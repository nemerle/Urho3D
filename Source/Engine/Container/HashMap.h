#ifndef HASHMAP
#define HASHMAP
#include <QtCore/QHash>
#include <unordered_map>
#include <unordered_set>
#ifdef USE_QT_HASHMAP
#endif
namespace Urho3D {
#ifdef USE_QT_HASHMAP
#define MAP_VALUE(i) (i.value())
#define MAP_KEY(i) (i.key())
template<typename T,typename U>
using HashMap = ::QHash<T,U> ;
#else
#define MAP_VALUE(i) (i->second)
#define MAP_KEY(i) (i->first)
template <typename T,typename U>
class HashMap : public std::unordered_map<T,U,std::hash<T> > {
public:
    typedef typename std::unordered_map<T,U>::iterator iterator;
    typedef typename std::unordered_map<T,U>::const_iterator const_iterator;

    constexpr bool contains(const T &v) const { return this->find(v)!=this->cend();}
    /// Erase an element if found.
    bool remove(const T& value)
    {
        iterator i = this->find(value);
        if (i == this->end())
            return false;
        this->erase(i);
        return true;
    }
    constexpr bool isEmpty() const { return this->empty(); }
};
template <typename T>
class HashSet : public std::unordered_set<T> {
public:
    typedef typename std::unordered_set<T>::iterator iterator;
    typedef typename std::unordered_set<T>::const_iterator const_iterator;

    constexpr bool contains(const T &v) const { return this->find(v)!=this->cend();}
    /// Erase an element if found.
    bool remove(const T& value)
    {
        iterator i = find(value);
        if (i == this->end())
            return false;
        this->erase(i);
        return true;
    }
    constexpr bool isEmpty() const { return this->empty(); }
};
#endif
}
#endif // HASHMAP

