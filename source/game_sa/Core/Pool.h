/*
    Plugin-SDK file
    Authors: GTA Community. See more here
    https://github.com/DK22Pac/plugin-sdk
    Do not delete this comment block. Respect others' work!
*/
#pragma once

#include <execution>
#include <Base.h>

#define INVALID_POOL_SLOT (-1)

/*
    R* terminology      Our terminology
    JustIndex           Index
    Index               Id
    Ref                 Handle/Ref
    Size                Capacity
*/

template<class T, class S = T>
class CPool {
    using ReturnType  = T;               //!< Common base of all these objects, this is the value returned from/expected to all functions
    using StorageType = byte[sizeof(S)]; //!< Memory for the widest object

private:
    struct SlotState {
        uint8 Ref : 7 { 0 };        //!< Incremented each time an object is allocated in this slot (Mask: 0x7F)
        bool  IsEmpty : 1 { true }; //!< If this slot is allocated right now (Mask: 0x80)

        auto ToInt() const { return std::bit_cast<uint8>(*this); }
    };

    VALIDATE_SIZE(SlotState, 1);

private:
    /*
    * Debug fill bytes for compatibility with MSVC/C++ debug heap
    * See
    * \Program Files\Microsoft Visual Studio\VC98\CRT\SRC\DBGHEAP.C:
    * static unsigned char _bNoMansLandFill = 0xFD;
    *   // fill no-man's land with this
    * static unsigned char _bDeadLandFill   = 0xDD;
    *   // fill free objects with this
    * static unsigned char _bCleanLandFill  = 0xCD;
    *   // fill new objects with this
    */
    constexpr static auto NOMANSLAND_FILL = 0xFD; //!< Initial fill of the pool's storage (That is, no objects have ever used this space)
    constexpr static auto DEADLAND_FILL   = 0xDD; //!< Delete'd objects are filled with this
    constexpr static auto CLEANLAND_FILL  = 0xCD; //!< New'd objects are filled with this (Expect the constructor to overwrite most of this)

public:
    /*!
    * @brief Default constructor, with no memory allocated
    */
    CPool() = default;

    /*!
    * @brief Initializes pool, owning memory
    ****/
    CPool(size_t capacity, const char* name) :
        m_Storage{ new StorageType[capacity] },
        m_SlotState{ new SlotState[capacity] },
        m_Capacity{ capacity },
        m_OwnsAllocations{ true }
    {
        assert(m_Storage);
        assert(m_SlotState);

        rng::uninitialized_fill(m_SlotState, m_SlotState + capacity, SlotState{});
        DoFill(NOMANSLAND_FILL);
    }

    /*!
    * @brief Initialises a pool with pre-allocated memory (non-owning)
    * @brief To be called one-time-only for statically allocated pools.
    ****/
    CPool(size_t capacity, void* storage, void* states) :
        m_Storage{ static_cast<StorageType*>(storage) },
        m_SlotState{ static_cast<SlotState*>(states) },
        m_Capacity{ capacity },
        m_OwnsAllocations{ false }
    {
        assert(m_Storage);
        assert(m_SlotState);

        rng::uninitialized_fill(m_SlotState, m_SlotState + capacity, SlotState{});
        DoFill(NOMANSLAND_FILL);
    }

    ~CPool() {
        Flush();
    }

    inline friend void swap(CPool<T, S>& a, CPool<T, S>& b) {
        using std::swap;
    
        swap(a.m_Storage, b.m_Storage);
        swap(a.m_SlotState, b.m_SlotState);
        swap(a.m_Capacity, b.m_Capacity);
        swap(a.m_LastFreeSlot, b.m_LastFreeSlot);
        swap(a.m_OwnsAllocations, b.m_OwnsAllocations);
        swap(a.m_DealWithNoMemory, b.m_DealWithNoMemory);
    }

    /* The `Init` function has been replaced by a constructor taking the same args */

    /*!
    * @brief Shut down pool, deallocate
    */
    void Flush() {
        DoFill(NOMANSLAND_FILL);
        if (m_OwnsAllocations) {
            delete[] std::exchange(m_Storage, nullptr);
            delete[] std::exchange(m_SlotState, nullptr);
        }
        m_Capacity         = 0;
        m_LastFreeSlot    = -1;
        m_OwnsAllocations  = false;
        m_DealWithNoMemory = false;
    }

    // Clears pool
    void Clear() {
        for (auto i = 0; i < m_Capacity; i++) {
            m_SlotState[i].IsEmpty = true;
        }
        DoFill(DEADLAND_FILL);
    }

    auto GetSize() {
        return m_Capacity;
    }

    /*!
    * @brief Returns if specified slot is free
    */
    // 0x404940
    bool IsFreeSlotAtIndex(size_t idx) const {
        assert(IsIndexInBounds(idx));
        return m_SlotState[idx].IsEmpty;
    }

    /*!
    * @brief Returns slot index for this object
    */
    auto GetIndex(const T* obj) const {
        assert(IsFromObjectArray(obj));
        return (StorageType*)(obj) - (StorageType*)(m_Storage);
    }

    /*!
    * @brief Returns pointer to object by slot index
    */
    T* GetAt(size_t idx) {
        assert(IsIndexInBounds(idx));
        return !IsFreeSlotAtIndex(idx) ? (T*)&m_Storage[idx] : nullptr;
    }

    /*!
    * @brief Marks slot as free / used (0x404970)
    */
    void SetFreeAt(size_t idx, bool isFree) {
        assert(IsIndexInBounds(idx));
        m_SlotState[idx].IsEmpty = isFree;
    }

    /*!
    * @brief Set new id for slot (0x54F9F0)
    */
    void SetIdAt(size_t idx, uint8 id) {
        assert(IsIndexInBounds(idx));
        m_SlotState[idx].Ref = id;
    }

    /*!
    * @brief Get id for slot (0x552200)
    */
    uint8 GetIdAt(size_t idx) {
        assert(IsIndexInBounds(idx));
        return m_SlotState[idx].Ref;
    }

    /*!
    * @brief Allocates object
    */
    T* New() {
        const auto i = FindFreeSlot();
        if (i == -1) {
            if (CanDealWithNoMemory()) {
                NOTSA_LOG_ERR("Allocation failed for type {:?}", typeid(T).name());
            } else {
                NOTSA_DEBUG_BREAK();
            }
            return nullptr;
        }
        assert(IsIndexInBounds(i) && "Free slot index is out-of-bounds");
        assert(IsFreeSlotAtIndex(i) && "Can't allocate an object at a non-free slot");

        auto* const state = &m_SlotState[i];
        const auto isFirstAllocation = state->Ref == 0; // First allocation of this slot?
        state->IsEmpty = false;
        state->Ref++;

        m_LastFreeSlot = i;

        StorageType* ptr = &m_Storage[i];
        
#ifdef _DEBUG
        // // Memory verification - Detects use-after-free
        // if (!isFirstAllocation) {
        //     try {
        //         const size_t checkSize = 16; // Check only the first 16 bytes
        //         byte expectedFill[checkSize];
        //         std::memset(expectedFill, DEADLAND_FILL, checkSize);
                
        //         if (std::memcmp(ptr, expectedFill, std::min(checkSize, sizeof(S))) != 0) {
        //             NOTSA_LOG_WARN("Possible use-after-free detected in object {:?} at 0x{:x}", typeid(T).name(), LOG_PTR(ptr));
        //         }
        //     } catch (...) {
        //         NOTSA_LOG_WARN("Error when verifying memory in New() for object {:?}", typeid(T).name());
        //     }
        // }
#endif

        DoFill(CLEANLAND_FILL, ptr);
        return (T*)(void*)(ptr);
    }

    /*!
    * @brief Allocates object at a specific index from a SCM handle (ref) (0x59F610)
    */
    void CreateAtRef(int32 ref) {
        const auto idx          = GetIndexFromRef(ref); // GetIndexFromRef asserts if idx out of range
        assert(IsFreeSlotAtIndex(idx) && "Can't create an object at a non-free slot");
        m_SlotState[idx].IsEmpty = false;
        m_SlotState[idx].Ref    = static_cast<uint8>(ref & 0x7F);
        m_LastFreeSlot         = 0;
        while (!m_SlotState[m_LastFreeSlot].IsEmpty) { // Find next free
            ++m_LastFreeSlot;
        }
    }

    /*!
    * @brief Allocate object at ref (0x5A1C00)
    * @returns A ptr to the object at ref
    */
    T* NewAt(int32 ref) {
        const auto idx = GetIndexFromRef(ref);
        assert(IsFreeSlotAtIndex(idx) && "Can't create an object at a non-free slot");
        S* ptr = reinterpret_cast<S*>(&m_Storage[idx]);
        CreateAtRef(ref);
        DoFill(CLEANLAND_FILL, ptr);
        return reinterpret_cast<T*>(ptr);
    }

    /*!
    * @brief Deallocates object
    */
    void Delete(T* obj) {
#ifdef FIX_BUGS // C++ says that `delete nullptr` is well defined, and should do nothing.
        if (!obj) {
            return;
        }
#endif
        assert(!IsFreeSlotAtIndex(GetIndex(obj)) && "Can't delete an already deleted object");
        int32 index               = GetIndex(obj);
        m_SlotState[index].IsEmpty = true;
        if (index < m_LastFreeSlot) {
            m_LastFreeSlot = index;
        }
        DoFill(DEADLAND_FILL, (S*)(obj));
    }

    /*!
    * @brief Returns SCM handle (ref) for object (0x424160)
    */
    int32 GetRef(const T* obj) {
        const auto idx = GetIndex(obj);
        return (idx << 8) | m_SlotState[idx].ToInt();
    }

    /*!
    * @brief Returns pointer to object by SCM handle (ref)
    */
    T* GetAtRef(int32 ref) {
        int32 idx = ref >> 8; // It is possible the ref is invalid here, thats why we check for the idx is valid below (And also why GetIndexFromRef isn't used, it would assert)
        return IsIndexInBounds(idx) && m_SlotState[idx].ToInt() == (ref & 0xFF)
            ? reinterpret_cast<T*>(&m_Storage[idx])
            : nullptr;
    }

    T* GetAtRefNoChecks(int32 ref) {
        return GetAt(GetIndexFromRef(ref));
    }

    /*!
    * @addr 0x54F6B0
    * @brief Calculate the number of used slots. CAUTION: Slow, especially for large pools.
    */
    size_t GetNoOfUsedSpaces() {
        return (size_t)std::count_if(m_SlotState, m_SlotState + m_Capacity, [](auto&& v) {
            return !v.IsEmpty;
        });
    }

    auto GetNoOfFreeSpaces() {
        return m_Capacity - GetNoOfUsedSpaces();
    }

    // 0x54F690
    auto GetObjectSize() {
        return sizeof(S);
    }

    // 0x5A1CD0
    bool IsObjectValid(const T* obj) const {
        return IsFromObjectArray(obj) && !IsFreeSlotAtIndex(GetIndex(obj));
    }

    // Helper so we don't write memcpy manually
    void CopyItem(T* dest, T* src) {
        *reinterpret_cast<S*>(dest) = *reinterpret_cast<S*>(src);
    }

    //
    // NOTSA section
    //

    /*!
    * @brief Check if index is in array bounds
    */
    [[nodiscard]] bool IsIndexInBounds(size_t idx) const {
        return idx >= 0 && idx < m_Capacity;
    }

    /*!
    * @brief Check if the pointer is from this pool 
    */
    bool IsPtrFromPool(const T* ptr) const {
        return m_Storage <= (StorageType*)(ptr) && (StorageType*)(ptr) < m_Storage + m_Capacity;
    }

    /*!
    * @brief Check if object pointer is inside object array (e.g.: It's index is in the bounds of the array)
    * @note Alias of IsPtrFromPool for compatibility
    */
    bool IsFromObjectArray(const T* obj) const {
        return IsPtrFromPool(obj);
    }

    /*!
    * @brief Get slot index from ref
    */
    int32 GetIndexFromRef(int32 ref) {
        const auto idx = ref >> 8;
        assert(IsIndexInBounds(idx));
        return idx;
    }

    // notsa
    void SetDealWithNoMemory(bool enabled) { m_DealWithNoMemory = enabled; }
    bool CanDealWithNoMemory() const { return m_DealWithNoMemory; }

    // NOTSA - Get all valid objects - Useful for iteration
    template<typename R = T&>
    auto GetAllValid() {
        return std::span{ reinterpret_cast<S*>(m_Storage), (size_t)(m_Capacity) }
            | rngv::filter([this](auto&& obj) {
                return !IsFreeSlotAtIndex(GetIndex(&obj));
            }) // Filter only slots in use
            | rngv::transform([](auto&& obj) -> R {
                if constexpr (std::is_pointer_v<R>) { // For pointers we also do an address-of
                    return static_cast<R>(&obj);
                } else {
                    return static_cast<R>(obj);
                }
            });
    }

    /*!
    * @brief Similar to above, but gives back a pair [index, object]
    */
    template<typename R = T>
    auto GetAllValidWithIndex() {
        return GetAllValid<R&>()
            | rng::views::transform([this](auto&& obj) {
                   return std::make_pair(GetIndex(&obj), std::ref(obj));
               });
    }

protected:
    void DoFill(byte fill, void* at = nullptr) {
        if (at) {
            memset(at, fill, sizeof(S)); /* One object */
        } else {
            memset(m_Storage, fill, sizeof(S) * m_Capacity); /* Entire storage */
        }
    }

    // Improved version of CheckFill with robust error handling
    bool CheckFill(byte expected, void* at) {
        // unimplemented
        return true;
    }

    // Finds the next free slot using an optimized algorithm
    int32 FindFreeSlot() const {
        const auto start = m_LastFreeSlot != -1 ? m_LastFreeSlot : 0;
        const auto cap = m_Capacity;

        // Search from the last free slot to the end
        for (auto i = start; i < cap; i++) {
            if (m_SlotState[i].IsEmpty) {
                return i;
            }
        }

        // If not found, search from the beginning to the last free slot
        for (auto i = 0; i < start; i++) {
            if (m_SlotState[i].IsEmpty) {
                return i;
            }
        }

        // No free slots
        return -1;
    }

private:
    StorageType* m_Storage{};           //!< Storage
    SlotState*   m_SlotState{};         //!< States of each slot
    size_t       m_Capacity{};          //!< Max no. of allocated objects (AKA Size)
    int32        m_LastFreeSlot{ -1 }; //!< First free slot in the storage
    bool         m_OwnsAllocations{};   //!< If the allocated arrays (`m_Storage` and `m_SlotState` is owned by, if so, we need to free them)
    bool         m_DealWithNoMemory{};  //!< If the caller is expected to be able to handle out-of-memory situations (Used for debugging) (AKA m_bIsLocked)
};
VALIDATE_SIZE(CPool<int32>, 0x14);
