#ifndef FILEWATCHER_H
#define FILEWATCHER_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <tchar.h>
#include <Pathcch.h>
#include <shlwapi.h>
#endif // WIN32

#if __unix__
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#endif // __unix__

#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <utility>
#include <vector>
#include <array>
#include <map>
#include <system_error>
#include <string>
#include <algorithm>
#include <future>

namespace filewatch {
	enum class Event {
		CREATED,
		DELETED,
		CHANGED,
		RENAMED_OLD,
		RENAMED_NEW
	};

	class FileWatch
	{
	public:

		FileWatch(std::string path, std::function<void(const std::string& file, const Event event_type)> callback) :
			_path(path),
			_callback(callback),
			_directory(get_directory(path))
		{
			init();
		}

		~FileWatch() 
		{
			destroy();
		}

		FileWatch(const FileWatch& other) : FileWatch(other._path, other._callback) {}

		FileWatch& operator=(const FileWatch& other)
		{
			if (this == &other) return *this;

			destroy();
			_path = other._path;
			_callback = other._callback;
			_directory = get_directory(other._path);
			init();
			return *this;
		}

		// Const member variables don't let me implent moves nicely, if moves are really wanted std::unique_ptr should be used and move that.
		FileWatch(FileWatch&&) = delete;
		FileWatch& operator=(FileWatch&&) & = delete;

	private:
		struct PathParts
		{
			PathParts(std::string _directory, std::string _filename) : directory(_directory), filename(_filename) {}
			std::string directory;
			std::string filename;
		};
		std::string _path;

		static constexpr std::size_t _buffer_size = 1024 * 256;

		// only used if watch a single file
		bool _watching_single_file = false;
		std::string _filename;

		std::atomic_bool _destroy{false};

		std::function<void(const std::string& file, const Event event_type)> _callback;

		std::thread _watch_thread;

		std::condition_variable _cv;
		std::mutex _callback_mutex;
		std::vector<std::pair<std::string, Event>> _callback_information;
		std::thread _callback_thread;

		std::promise<void> _running;
#ifdef _WIN32
		HANDLE _directory = nullptr;
		HANDLE _close_event = nullptr;

		const DWORD _listen_filters =
			FILE_NOTIFY_CHANGE_SECURITY |
			FILE_NOTIFY_CHANGE_CREATION |
			FILE_NOTIFY_CHANGE_LAST_ACCESS |
			FILE_NOTIFY_CHANGE_LAST_WRITE |
			FILE_NOTIFY_CHANGE_SIZE |
			FILE_NOTIFY_CHANGE_ATTRIBUTES |
			FILE_NOTIFY_CHANGE_DIR_NAME |
			FILE_NOTIFY_CHANGE_FILE_NAME;

		const std::map<DWORD, Event> _event_type_mapping = {
			std::pair(FILE_ACTION_ADDED, Event::CREATED),
			std::pair(FILE_ACTION_REMOVED, Event::DELETED),
			std::pair(FILE_ACTION_MODIFIED, Event::CHANGED),
			std::pair(FILE_ACTION_RENAMED_OLD_NAME, Event::RENAMED_OLD),
			std::pair(FILE_ACTION_RENAMED_NEW_NAME, Event::RENAMED_NEW)
		};
#endif // WIN32

#if __unix__
		struct FolderInfo {
			int folder;
			int watch;
		};

		FolderInfo _directory;

		const std::uint32_t _listen_filters = IN_MODIFY | IN_CREATE | IN_DELETE;

		const static std::size_t event_size = (sizeof(struct inotify_event));
#endif // __unix__

		void init()
		{
#ifdef _WIN32
			_close_event = CreateEvent(nullptr, true, false, nullptr);
			if (!_close_event) 
				throw std::system_error(GetLastError(), std::system_category());
#endif // WIN32
			_callback_thread = std::move(std::thread([this]() {
				try 
				{
					callback_thread();
				}
				catch (...) 
				{
					try 
					{
						_running.set_exception(std::current_exception());
					}
					catch (...) {} // set_exception() may throw too
				}
			}));

			_watch_thread = std::move(std::thread([this]() {
				try 
				{
					monitor_directory();
				}
				catch (...) 
				{
					try {
						_running.set_exception(std::current_exception());
					}
					catch (...) {} // set_exception() may throw too
				}
			}));

			std::future<void> future = _running.get_future();
			future.get(); //block until the monitor_directory is up and running
		}

		void destroy()
		{
			_destroy = true;
			_running = std::promise<void>();
#ifdef _WIN32
			SetEvent(_close_event);
#elif __unix__
			inotify_rm_watch(_directory.folder, _directory.watch);
#endif // __unix__
			_cv.notify_all();
			_watch_thread.join();
			_callback_thread.join();
#ifdef _WIN32
			CloseHandle(_directory);
#elif __unix__
			close(_directory.folder);
#endif // __unix__
		}

		const PathParts split_directory_and_file(const std::string& path) const
		{
			const auto predict = [](typename std::string::value_type character) {
#ifdef _WIN32
				return character == _T('\\') || character == _T('/');
#elif __unix__
				return character == '/';
#endif // __unix__
			};
#ifdef _WIN32
#define _UNICODE
			const std::string this_directory = _T("./");
#elif __unix__
			const std::string this_directory = "./";
#endif // __unix__

			const std::string::const_iterator pivot = std::find_if(path.rbegin(), path.rend(), predict).base();
			//if the path is something like "test.txt" there will be no directoy part, however we still need one, so insert './'
			const std::string extracted_directory = std::string(path.begin(), pivot);
			std::string directory = (extracted_directory.size() > 0) ? extracted_directory : this_directory;

			const std::string filename = std::string(pivot, path.end());
			return PathParts(directory, filename);
		}

		bool pass_filter(const std::string& file_path)
		{
			if (_watching_single_file) 
			{
				const std::string extracted_filename = { split_directory_and_file(file_path).filename };
				//if we are watching a single file, only that file should trigger action
				return extracted_filename == _filename;
			}

			return true;
		}

#ifdef _WIN32
		HANDLE get_directory(const std::string& path)
		{
			DWORD file_info = GetFileAttributes(path.c_str());
			if (file_info == INVALID_FILE_ATTRIBUTES)
				throw std::system_error(GetLastError(), std::system_category());

			std::string watch_path;
			_watching_single_file = (file_info & FILE_ATTRIBUTE_DIRECTORY) == false;
			if (_watching_single_file)
			{
				const auto parsed_path = split_directory_and_file(path);
				_filename = parsed_path.filename;
				watch_path = parsed_path.directory;
			}
			else
			{
				watch_path = path;
			}

			HANDLE directory = CreateFile(
				watch_path.c_str(),                                     // pointer to the file name
				FILE_LIST_DIRECTORY,                                    // access (read/write) mode
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, // share mode
				nullptr,                                                // security descriptor
				OPEN_EXISTING,                                          // how to create
				FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,      // file attributes
				nullptr);                                               // file with attributes to copy

			if (directory == INVALID_HANDLE_VALUE)
				throw std::system_error(GetLastError(), std::system_category());

			return directory;
		}

		std::string unicode_to_string(const std::wstring& unicode_string)
		{
			int unicode_len = (int)unicode_string.length() + 1;
			int len = WideCharToMultiByte(CP_ACP, 0, unicode_string.c_str(), unicode_len, 0, 0, 0, 0);
			char* buffer = new char[len];
			WideCharToMultiByte(CP_ACP, 0, unicode_string.c_str(), unicode_len, buffer, len, 0, 0);
			std::string translated_string(buffer);
			delete[] buffer;

			return translated_string;
		}

		void monitor_directory()
		{
			std::vector<BYTE> buffer(_buffer_size);
			DWORD bytes_returned = 0;
			OVERLAPPED overlapped_buffer{ 0 };

			overlapped_buffer.hEvent = CreateEvent(nullptr, true, false, nullptr);
			if (!overlapped_buffer.hEvent) 
				std::cerr << "Error creating monitor event" << std::endl;

			std::array<HANDLE, 2> handles{ overlapped_buffer.hEvent, _close_event };

			bool async_pending = false;
			_running.set_value();
			do 
			{
				std::vector<std::pair<std::string, Event>> parsed_information = {};
				ReadDirectoryChangesW(_directory, buffer.data(), buffer.size(), true, _listen_filters, &bytes_returned, &overlapped_buffer, nullptr);

				async_pending = true;

				switch (WaitForMultipleObjects(2, handles.data(), false, INFINITE))
				{
					case WAIT_OBJECT_0:
					{
						if (!GetOverlappedResult(_directory, &overlapped_buffer, &bytes_returned, true)) 
							throw std::system_error(GetLastError(), std::system_category());

						async_pending = false;

						if (bytes_returned == 0) break;

						FILE_NOTIFY_INFORMATION* file_information = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(&buffer[0]);
						do
						{
							std::wstring temp_changed_file(file_information->FileName, file_information->FileNameLength / 2);
							std::string changed_file(temp_changed_file.size(), static_cast<typename std::string::value_type>('\0'));
							for (size_t k = 0; k < changed_file.size(); ++k)
								changed_file[k] = static_cast<typename std::string::value_type>(temp_changed_file[k]);

							if (pass_filter(changed_file))
								parsed_information.emplace_back(std::string(changed_file), _event_type_mapping.at(file_information->Action));

							if (file_information->NextEntryOffset == 0) break;

							file_information = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<BYTE*>(file_information) + file_information->NextEntryOffset);
						} while (true);
						break;
					}
					case WAIT_OBJECT_0 + 1:
						// quit
						break;
					case WAIT_FAILED:
						break;
				}

				//dispatch callbacks
				std::lock_guard<std::mutex> lock(_callback_mutex);
				_callback_information.insert(_callback_information.end(), parsed_information.begin(), parsed_information.end());
				_cv.notify_all();
			} while (_destroy == false);

			if (async_pending)
			{
				//clean up running async io
				CancelIo(_directory);
				GetOverlappedResult(_directory, &overlapped_buffer, &bytes_returned, true);
			}
		}
#endif // WIN32

#if __unix__

		bool is_file(const std::string& path) const
		{
			struct stat statbuf = {};
			if (stat(path.c_str(), &statbuf) != 0)
				throw std::system_error(errno, std::system_category());

			return S_ISREG(statbuf.st_mode);
		}

		FolderInfo get_directory(const std::string& path)
		{
			const int folder = inotify_init();
			if (folder < 0)
				throw std::system_error(errno, std::system_category());

			_watching_single_file = is_file(path);

			std::string watch_path;
			{
				if (_watching_single_file)
				{
					const filewatch::FileWatch::PathParts parsed_path = split_directory_and_file(path);
					_filename = parsed_path.filename;
					watch_path = parsed_path.directory;
				}
				else
				{
					watch_path = path;
				}
			}

			const auto watch = inotify_add_watch(folder, watch_path.c_str(), IN_MODIFY | IN_CREATE | IN_DELETE);
			if (watch < 0)
				throw std::system_error(errno, std::system_category());

			return { folder, watch };
		}

		void monitor_directory()
		{
			std::vector<char> buffer(_buffer_size);

			_running.set_value();
			while (_destroy == false)
			{
				const auto length = read(_directory.folder, static_cast<void*>(buffer.data()), buffer.size());
				if (length > 0)
				{
					int i = 0;
					std::vector<std::pair<std::string, Event>> parsed_information;
					while (i < length)
					{
						struct inotify_event* event = reinterpret_cast<struct inotify_event*>(&buffer[i]); // NOLINT
						if (event->len)
						{
							const std::string changed_file(event->name);
							if (pass_filter(changed_file))
							{
								if (event->mask & IN_CREATE)
									parsed_information.emplace_back(std::string(changed_file), Event::CREATED);
								else if (event->mask & IN_DELETE)
									parsed_information.emplace_back(std::string(changed_file), Event::DELETED);
								else if (event->mask & IN_MODIFY)
									parsed_information.emplace_back(std::string(changed_file), Event::CHANGED);
							}
						}
						i += event_size + event->len;
					}

					//dispatch callbacks
					std::lock_guard<std::mutex> lock(_callback_mutex);
					_callback_information.insert(_callback_information.end(), parsed_information.begin(), parsed_information.end());
					_cv.notify_all();
				}
			}
		}
#endif // __unix__

		void callback_thread()
		{
			while (_destroy == false) 
			{
				std::unique_lock<std::mutex> lock(_callback_mutex);
				if (_callback_information.empty() && _destroy == false) 
					_cv.wait(lock, [this] { return _callback_information.size() > 0 || _destroy; });

				std::vector<std::pair<std::string, Event>> callback_information = {};
				std::swap(callback_information, _callback_information);
				lock.unlock();

				for (const auto& file : callback_information)
				{
					if (!_callback) continue;

					try
					{
						_callback(file.first, file.second);
					}
					catch (const std::exception&)
					{
					}
				}
			}
		}
	};
}
#endif