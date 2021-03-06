#pragma once

#include <vector>
#include <thread>
#include <atomic>
#include <shared_mutex>

namespace ACIDList {

	enum states {
		REMOVED,
		BEGIN,
		VALID,
		END,
	};

	class RWLock {
	 protected:
		 template <typename DATA>
		 friend class List;

		 template <typename DATA>
		 friend class Node;

		 template <typename DATA>
		 friend class FreeNode;

		 template <typename DATA>
		 friend class Iterator;

		 template <typename DATA>
		 friend class FreeList;

		RWLock() : val(0), bit(1 << 31) {}

		void rlock() {
			while (true) {
				uint32_t old = this->val;
				
				if (!(old & this->bit) && this->val.compare_exchange_strong(old, old + 1)) return;
				std::this_thread::yield();
			}
		}

		void wlock() {
			while (true) {
				uint32_t old = this->val;
				
				if (!(old & this->bit) &&
					this->val.compare_exchange_strong(old, old | this->bit)) {
					break;
				}
				std::this_thread::yield();
			}
			
			while (true) {
				if (this->val == this->bit) break;
				std::this_thread::yield();
			}
		}

		void unlock() {
			if (this->val == this->bit) val = 0;
			else this->val--;
		}

		std::atomic<uint32_t> val;
		uint32_t bit;
	};

	template <typename DATA>
	class Node {
	 protected:
		template <typename DATA>
		friend class Iterator;

		template <typename DATA>
		friend class List;

		template <typename DATA>
		friend class FreeList;

		friend class RWLock;

		using data_type = DATA;
		using state_for_node = states;
		using size_type = std::size_t;
		using cur_list = List<DATA>;
		using rw_lock = RWLock;

		Node(states state, cur_list *clist) : list(clist), data(), prev(nullptr), next(nullptr), state(state), ref_count(0) {}

		Node(data_type value, cur_list *clist) :list(clist), data(value), prev(nullptr), next(nullptr), state(states::VALID), 
			ref_count(0) {}

		void destroy() {
			this->list->freelock.rlock();
			size_type ref = this->ref_count--;
			if (ref == 0) this->list->myfreelist->push(this);
			this->list->freelock.unlock();
		}
		
		cur_list *list;
		data_type data;
		
		Node *prev;
		Node *next;
		
		std::atomic<states> state;
		std::atomic<bool> already;
		std::atomic<size_type> ref_count;
		rw_lock lock;
	};

	template <typename DATA>
	class FreeNode {
	 protected:
		using node = Node<DATA>;

		template<typename DATA>
		friend class FreeList;

		friend class RWLock;

		FreeNode(node* tmp) : ptr(tmp), next(nullptr) {}

		node *ptr;
		FreeNode *next;
	};

	template <typename DATA>
	class FreeList {
	 protected:
		using list = List<DATA>;
		using fnode = FreeNode<DATA>;
		using lnode = Node<DATA>;

		template<typename DATA>
		friend class List;

		template<typename DATA>
		friend class Node;

		FreeList(list *tmp) : mylist(tmp) {
			this->cur_thread = std::thread(&FreeList::clear_list, this);
		}

		~FreeList() {
			this->clear = true;
			this->cur_thread.join();
		}

		void push(lnode* node) {
			fnode *pnode = new fnode(node);

			while (!this->main.compare_exchange_strong(pnode->next, pnode)) {
				pnode->next = this->main.load();
			}
		}

		void remove(fnode* prev, fnode* node) {
			prev->next = node->next;
			delete node;
		}

		void destroy_node(fnode* node) {
			lnode* left = node->ptr->prev;
			lnode* right = node->ptr->next;

			if (left) left->destroy();
			if (right) right->destroy();

			delete node->ptr;
			delete node;
		}

		void clear_list() {
			while (!this->clear || this->main.load()) {
				this->mylist->freelock.wlock();
				fnode* tmp = this->main;
				this->mylist->freelock.unlock();

				if (tmp) {
					fnode* prev = tmp;
					for (fnode* next_node = tmp; next_node;) {
						fnode* cur_node = next_node;
						next_node = next_node->next;

						if (cur_node->ptr->ref_count != 0 || cur_node->ptr->already == true) {
							remove(prev, cur_node);
						}
						else {
							cur_node->ptr->already = true;
							prev = cur_node;
						}
					}

					this->mylist->freelock.wlock();
					fnode* temp = this->main;

					if (tmp == temp) this->main = nullptr;

					this->mylist->freelock.unlock();

					prev = temp;
					for (fnode* next_node = temp; next_node != tmp;) {
						fnode* cur_node = next_node;
						next_node = next_node->next;

						if (cur_node->ptr->already == true) remove(prev, cur_node);
						else prev = cur_node;
					}

					prev->next = nullptr;

					for (fnode* next_node = tmp; next_node;) {
						fnode* cur_node = next_node;
						next_node = next_node->next;
						destroy_node(cur_node);
					}
				}
				if (!this->clear) {
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
				}
			}
		}

		list *mylist;
		std::atomic<fnode*>	main;
		std::thread cur_thread;
		std::atomic<bool> clear;
	};

	template <typename DATA>
	class Iterator {
	public:
		using iterator_category = std::forward_iterator_tag;
		using difference_type = std::ptrdiff_t;
		using data_type = DATA;
		using reference = data_type&;
		using pointer = data_type*;
		using node = Node<DATA>;
		using list = List<DATA>;

		template <typename DATA>
		friend class List;

		template <typename DATA>
		friend class FreeList;

		friend class RWLock;

		Iterator(const Iterator &other) noexcept {
			this->ptr = other.ptr;
			this->ptr->ref_count++;
			this->mylist = other.mylist;
		}

		Iterator(node* value, list* list) noexcept {
			this->ptr = value;
			this->ptr->ref_count++;
			this->mylist = list;
		}

		~Iterator() {
			this->ptr->destroy();
		}

		reference operator*() const {
			this->ptr->lock.rlock();
			return this->ptr->data;
			this->ptr->lock.unlock();
		}

		Iterator& operator=(const Iterator &other) {
			this->ptr->lock.wlock();
			
			if (this->ptr == other.ptr) {
				this->ptr->lock.unlock();
				return *this;
			}
			
			other.ptr->lock.wlock();

			node *tmp = this->ptr;
			this->ptr = other.ptr;
			this->ptr->ref_count++;
			this->mylist = other.mylist;

			tmp->lock.unlock();
			this->ptr->lock.unlock();

			tmp->destroy();
			return *this;
		}

		Iterator& operator=(Iterator &&other) {
			this->ptr->lock.wlock();
			if (this->ptr == other.ptr) {
				this->ptr->lock.unlock();
				return *this;
			}

			other.ptr->lock.wlock();

			node *tmp = this->ptr;
			this->ptr= other.ptr;
			this->ptr->ref_count++;
			this->mylist = other.mylist;

			tmp->lock.unlock();
			this->ptr->lock.unlock();

			tmp->destroy();
			return *this;
		}

		data_type get() {
			this->ptr->lock.rlock();
			data_type tmp = this->ptr->data;
			this->ptr->lock.unlock();
			
			return tmp;
		}

		node* get_pointer() {
			return this->ptr;
		}

		void set(data_type value) {
			this->ptr->lock.wlock();
			this->ptr->data = value;
			this->ptr->lock.unlock();
		}

		Iterator& operator++() {
			plus();
			return *this;
		}

		Iterator operator++(int) {
			Iterator tmp = *this;
			plus();
			return tmp;
		}

		Iterator& operator--() {
			minus();
			return *this;
		}

		Iterator operator--(int) {
			Iterator tmp = *this;
			minus();
			return tmp;
		}

		bool operator==(const Iterator& other) {
			return other.ptr == this->ptr;
		}

		bool operator!=(const Iterator& other) {
			return other.ptr!= this->ptr;
		}

	private:
		void plus() {
			if (this->ptr && this->ptr->state != states::END) {
				node* tmp;
				this->mylist->freelock.rlock();

				tmp = this->ptr;
				this->ptr= this->ptr->next;
				this->ptr->ref_count++;
				
				this->mylist->freelock.unlock();
				tmp->destroy();
			}
		}

		void minus() {
			if (this->ptr && this->ptr->state != states::BEGIN) {
				node* tmp;
				this->mylist->freelock.rlock();

				tmp = this->ptr;
				this->ptr = this->ptr->prev;
				this->ptr->ref_count++;

				this->mylist->freelock.unlock();
				tmp->destroy();
			}
		}

		node *ptr;
		list *mylist;
	};

	template <typename DATA>
	class List {
	 public:
		using size_type = std::size_t;
		using list_type = DATA;
		using data_type = Node<list_type>;
		using reference = list_type&;
		using const_reference = const list_type&;
		using iterator = Iterator<list_type>;
		using freelist = FreeList<DATA>;
		using rw_lock = RWLock;

		template <typename DATA>
		friend class FreeList;

		template <typename DATA>
		friend class Node;

		template <typename DATA>
		friend class Iterator;

		friend class RWLock;

		List(std::initializer_list<list_type> list) : List() {
			for (auto it : list) push_back(it);
		}

		List() {
			this->last = new data_type(states::END, this);
			this->root = new data_type(states::BEGIN, this);
			this->myfreelist= new freelist(this);

			this->last->ref_count++;
			this->root->ref_count++;

			this->last->prev = this->root;
			this->root->next = this->last;
		}

		~List() {
			delete this->myfreelist;
			data_type* tmp = this->root;
			
			while (tmp != this->last) {
				data_type* prev = tmp;
				tmp = tmp->next;
				delete prev;
			}
			
			delete tmp;
		}

		size_type size() {
			return this->size_;
		}

		iterator begin() {
			this->root->lock.rlock();
			iterator b(this->root->next, this);
			this->root->lock.unlock();
			return b;
		}

		iterator end() {
			this->last->lock.rlock();
			iterator e(this->last, this);
			this->last->lock.unlock();
			return e;
		}

		void push_front(list_type value) {
			this->root->lock.wlock();
			data_type* tmp = this->root->next;
			tmp->lock.wlock();

			data_type* new_node = new data_type(value, this);
			new_node->prev = this->root;
			new_node->next = tmp;
			new_node->ref_count++;
			new_node->ref_count++;


			tmp->prev = new_node;
			this->root->next = new_node;
			size_++;

			this->root->lock.unlock();
			tmp->lock.unlock();
		}

		void push_back(list_type value) {
			data_type* left;
			for (bool retry = true; retry;) {
				this->last->lock.wlock();
				left = this->last->prev;
				left->ref_count++;
				this->last->lock.unlock();

				left->lock.wlock();
				this->last->lock.wlock();

				if (left->next == this->last && this->last->prev == left) {
					data_type* new_node = new data_type(value, this);
						
					new_node->prev = left;
					new_node->next = this->last;
					new_node->ref_count++;
					new_node->ref_count++;


					left->next = new_node;
					this->last->prev = new_node;
					this->size_++;

					retry = false;
				}
				
				left->lock.unlock();
				this->last->lock.unlock();
				left->destroy();
			}
		}

		void insert(iterator &it, list_type value) {
			data_type* left = it.ptr;
			
			if (left->state == states::END) push_back(value);
			else if (left->state == states::BEGIN) push_front(value);
			else {
				left->lock.wlock();
				
				if (left->state == states::REMOVED) {
					left->lock.unlock();
					return;
				}

				data_type* right = left->next;
				right->lock.wlock();

				data_type* new_node = new data_type(value, this);
				
				new_node->ref_count++;
				new_node->ref_count++;
				new_node->prev = left;
				new_node->next = right;

				left->next = new_node;
				right->prev = new_node;

				this->size_++;

				left->lock.unlock();
				right->lock.unlock();
			}
		}

		iterator find(list_type value) {
			this->root->lock.rlock();
			data_type* tmp = this->root;
			tmp = tmp->next;
			this->root->lock.unlock();
			
			while (tmp != this->last && tmp->data != value) {
				tmp->lock.rlock();
				data_type* right = tmp->next;
				tmp->lock.unlock();
				tmp = right;
			}
			
			return iterator(tmp, this);
		}

		void erase(iterator it) {
			data_type* node = it.ptr;

			if (this->size_ == 0 || node->state != states::VALID) return;

			for (bool retry = true; retry;) {
				
				node->lock.rlock();
				if (node->state == states::REMOVED) {
					node->lock.unlock();
					return;
				}

				data_type *left = node->prev;
				data_type *right = node->next;
				left->ref_count++;
				right->ref_count++;
				node->lock.unlock();

				left->lock.wlock();
				node->lock.rlock();
				right->lock.wlock();

				if (left->next == node && right->prev == node) {
					node->state = states::REMOVED;
						
					node->ref_count--;
					node->ref_count--;

					left->next = right;
					right->prev = left;
					left->ref_count++;
					right->ref_count++;

					this->size_--;
					retry = false;
				}
				
				left->lock.unlock();
				node->lock.unlock();
				right->lock.unlock();
				
				left->destroy();
				right->destroy();
			}
		}

		void pop_back() {
			if (this->size_ != 0) {
				this->last->lock.wlock();
				iterator it = iterator(this->last->prev);
				this->last->lock.unlock();
				erase(it);
			}
		}

	 private:
		data_type* root;
		data_type* last;
		std::atomic<size_type> size_;
		freelist *myfreelist;
		rw_lock freelock;
	};
}