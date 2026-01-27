/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <haze.hpp>
#include <haze/ptp_data_builder.hpp>
#include <haze/ptp_data_parser.hpp>
#include <haze/ptp_responder_types.hpp>
#include "haze/threaded_file_transfer.hpp"

namespace haze {

    Result PtpResponder::GetDeviceInfo(PtpDataParser &dp) {
        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        /* Write the device info data. */
        R_TRY(db.WriteVariableLengthData(m_request_header, [&] () {
            R_TRY(db.Add(MtpStandardVersion));
            R_TRY(db.Add(MtpVendorExtensionId));
            R_TRY(db.Add(MtpStandardVersion));
            R_TRY(db.AddString(MtpVendorExtensionDesc));
            R_TRY(db.Add(MtpFunctionalModeDefault));
            R_TRY(db.AddArray(SupportedOperationCodes, util::size(SupportedOperationCodes)));
            R_TRY(db.AddArray(SupportedEventCodes, util::size(SupportedEventCodes)));
            R_TRY(db.AddArray(SupportedDeviceProperties, util::size(SupportedDeviceProperties)));
            R_TRY(db.AddArray(SupportedCaptureFormats, util::size(SupportedCaptureFormats)));
            R_TRY(db.AddArray(SupportedPlaybackFormats, util::size(SupportedPlaybackFormats)));
            R_TRY(db.AddString(MtpDeviceManufacturer));
            R_TRY(db.AddString(MtpDeviceModel));
            R_TRY(db.AddString(GetFirmwareVersion()));
            R_TRY(db.AddString(GetSerialNumber()));

            R_SUCCEED();
        }));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::OpenSession(PtpDataParser &dp) {
        R_TRY(dp.Finalize());

        /* Close, if we're already open. */
        this->ForceCloseSession();

        /* Initialize the database. */
        m_session_open = true;
        m_object_database.Initialize(m_object_heap);

        /* Create the root storages. */
        for (const auto& fs : m_fs_entries) {
            const auto name = fs.impl->GetName();
            const auto storage_id = fs.storage_id;

            PtpObject *object;
            R_TRY(m_object_database.CreateOrFindObject("", name, PtpGetObjectHandles_RootParent, storage_id, std::addressof(object)));

            /* Register the root storages. */
            m_object_database.RegisterObject(object, storage_id);
        }

        WriteCallbackSession(CallbackType_OpenSession);

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::CloseSession(PtpDataParser &dp) {
        R_TRY(dp.Finalize());

        this->ForceCloseSession();

        WriteCallbackSession(CallbackType_CloseSession);

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::GetStorageIds(PtpDataParser &dp) {
        R_TRY(dp.Finalize());

        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        std::vector<u32> storage_ids;
        for (const auto& e : m_fs_entries) {
            storage_ids.emplace_back(e.storage_id);
        }

        /* Write the storage ID array. */
        R_TRY(db.WriteVariableLengthData(m_request_header, [&] {
            R_RETURN(db.AddArray(storage_ids.data(), std::size(storage_ids)));
        }));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::GetStorageInfo(PtpDataParser &dp) {
        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));
        PtpStorageInfo storage_info(DefaultStorageInfo);

        /* Get the storage ID the client requested information for. */
        u32 storage_id;
        R_TRY(dp.Read(std::addressof(storage_id)));
        R_TRY(dp.Finalize());

        const auto it = std::find_if(m_fs_entries.cbegin(), m_fs_entries.cend(), [storage_id](auto& e){
            return storage_id == e.storage_id;
        });
        R_UNLESS(it != m_fs_entries.cend(), haze::ResultInvalidStorageId());

        s64 total_space, free_space;
        R_TRY(Fs(storage_id).GetTotalSpace("/", std::addressof(total_space)));
        R_TRY(Fs(storage_id).GetFreeSpace("/", std::addressof(free_space)));

        storage_info.max_capacity         = total_space;
        storage_info.free_space_in_bytes  = free_space;
        storage_info.free_space_in_images = 0;
        storage_info.storage_description = it->impl->GetDisplayName();

        /* Write the storage info data. */
        R_TRY(db.WriteVariableLengthData(m_request_header, [&] () {
            R_TRY(db.Add(storage_info.storage_type));
            R_TRY(db.Add(storage_info.filesystem_type));
            R_TRY(db.Add(storage_info.access_capability));
            R_TRY(db.Add(storage_info.max_capacity));
            R_TRY(db.Add(storage_info.free_space_in_bytes));
            R_TRY(db.Add(storage_info.free_space_in_images));
            R_TRY(db.AddString(storage_info.storage_description));
            R_TRY(db.AddString(storage_info.volume_label));

            R_SUCCEED();
        }));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::GetObjectHandles(PtpDataParser &dp) {
        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        /* Get the object ID the client requested enumeration for. */
        u32 storage_id, object_format_code, association_object_handle;
        R_TRY(dp.Read(std::addressof(storage_id)));
        R_TRY(dp.Read(std::addressof(object_format_code)));
        R_TRY(dp.Read(std::addressof(association_object_handle)));
        R_TRY(dp.Finalize());

        /* Handle top-level requests. */
        if (storage_id == PtpGetObjectHandles_AllStorage) {
            storage_id = StorageId_DefaultStorage;
        }

        /* Rewrite requests for enumerating storage directories. */
        if (association_object_handle == PtpGetObjectHandles_RootParent) {
            association_object_handle = storage_id;
        }

        /* Check if we know about the object. If we don't, it's an error. */
        auto * const obj = m_object_database.GetObjectById(association_object_handle);
        R_UNLESS(obj != nullptr, haze::ResultInvalidObjectId());

        /* Try to read the object as a directory. */
        FsDir dir;
        R_TRY(Fs(obj).OpenDirectory(obj->GetName(), FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, std::addressof(dir)));

        /* Ensure we maintain a clean state on exit. */
        ON_SCOPE_EXIT { Fs(obj).CloseDirectory(std::addressof(dir)); };

        /* Count how many entries are in the directory. */
        s64 entry_count = 0;
        R_TRY(Fs(obj).GetDirectoryEntryCount(std::addressof(dir), std::addressof(entry_count)));

        /* Begin writing. */
        R_TRY(db.AddDataHeader(m_request_header, sizeof(u32) + (entry_count * sizeof(u32))));
        R_TRY(db.Add(static_cast<u32>(entry_count)));

        /* Enumerate the directory, writing results to the data builder as we progress. */
        /* TODO: How should we handle the directory contents changing during enumeration? */
        /* Is this even feasible to handle? */
        while (true) {
            /* Get the next batch. */
            s64 read_count = 0;
            R_TRY(Fs(obj).ReadDirectory(std::addressof(dir), std::addressof(read_count), DirectoryReadSize, m_buffers->file_system_entry_buffer));

            /* Write to output. */
            for (s64 i = 0; i < read_count; i++) {
                const char *name = m_buffers->file_system_entry_buffer[i].name;
                u32 handle;

                R_TRY(m_object_database.CreateAndRegisterObjectId(obj->GetName(), name, obj->GetObjectId(), obj->GetStorageId(), std::addressof(handle)));
                R_TRY(db.Add(handle));
            }

            /* If we read fewer than the batch size, we're done. */
            if (read_count < DirectoryReadSize) {
                break;
            }
        }

        /* Flush the data response. */
        R_TRY(db.Commit());

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::GetObjectInfo(PtpDataParser &dp) {
        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        /* Get the object ID the client requested info for. */
        u32 object_id;
        R_TRY(dp.Read(std::addressof(object_id)));
        R_TRY(dp.Finalize());

        /* Check if we know about the object. If we don't, it's an error. */
        auto * const obj = m_object_database.GetObjectById(object_id);
        R_UNLESS(obj != nullptr, haze::ResultInvalidObjectId());

        /* Build info about the object. */
        PtpObjectInfo object_info(DefaultObjectInfo);

        const auto it = std::find_if(m_fs_entries.cbegin(), m_fs_entries.cend(), [object_id](auto& e){
            return object_id == e.storage_id;
        });

        if (it != m_fs_entries.cend()) {
            /* The SD Card directory has some special properties. */
            object_info.object_format    = PtpObjectFormatCode_Association;
            object_info.association_type = PtpAssociationType_GenericFolder;
            object_info.filename         = it->impl->GetDisplayName();
        } else {
            /* Figure out what type of object this is. */
            FsDirEntryType entry_type;
            R_TRY(Fs(obj).GetEntryType(obj->GetName(), std::addressof(entry_type)));

            /* Get the size, if we are requesting info about a file. */
            s64 size = 0;
            if (entry_type == FsDirEntryType_File) {
                FsFile file;
                R_TRY(Fs(obj).OpenFile(obj->GetName(), FsOpenMode_Read, std::addressof(file)));

                /* Ensure we maintain a clean state on exit. */
                ON_SCOPE_EXIT { Fs(obj).CloseFile(std::addressof(file)); };

                R_TRY(Fs(obj).GetFileSize(std::addressof(file), std::addressof(size)));
            }

            object_info.filename               = std::strrchr(obj->GetName(), '/') + 1;
            object_info.object_compressed_size = size;
            object_info.parent_object          = obj->GetParentId();

            if (entry_type == FsDirEntryType_Dir) {
                object_info.object_format    = PtpObjectFormatCode_Association;
                object_info.association_type = PtpAssociationType_GenericFolder;
            } else {
                object_info.object_format    = PtpObjectFormatCode_Undefined;
                object_info.association_type = PtpAssociationType_Undefined;
            }
        }

        /* Write the object info data. */
        R_TRY(db.WriteVariableLengthData(m_request_header, [&] () {
            R_TRY(db.Add(object_info.storage_id));
            R_TRY(db.Add(object_info.object_format));
            R_TRY(db.Add(object_info.protection_status));
            R_TRY(db.Add(object_info.object_compressed_size));
            R_TRY(db.Add(object_info.thumb_format));
            R_TRY(db.Add(object_info.thumb_compressed_size));
            R_TRY(db.Add(object_info.thumb_width));
            R_TRY(db.Add(object_info.thumb_height));
            R_TRY(db.Add(object_info.image_width));
            R_TRY(db.Add(object_info.image_height));
            R_TRY(db.Add(object_info.image_depth));
            R_TRY(db.Add(object_info.parent_object));
            R_TRY(db.Add(object_info.association_type));
            R_TRY(db.Add(object_info.association_desc));
            R_TRY(db.Add(object_info.sequence_number));
            R_TRY(db.AddString(object_info.filename));
            R_TRY(db.AddString(object_info.capture_date));
            R_TRY(db.AddString(object_info.modification_date));
            R_TRY(db.AddString(object_info.keywords));

            R_SUCCEED();
        }));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::GetObject(PtpDataParser &dp) {
        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        /* Get the object ID the client requested. */
        u32 object_id;
        R_TRY(dp.Read(std::addressof(object_id)));
        R_TRY(dp.Finalize());

        /* Check if we know about the object. If we don't, it's an error. */
        auto * const obj = m_object_database.GetObjectById(object_id);
        R_UNLESS(obj != nullptr, haze::ResultInvalidObjectId());

        /* Lock the object as a file. */
        FsFile file;
        R_TRY(Fs(obj).OpenFile(obj->GetName(), FsOpenMode_Read, std::addressof(file)));

        /* Ensure we maintain a clean state on exit. */
        ON_SCOPE_EXIT { Fs(obj).CloseFile(std::addressof(file)); };

        /* Get the file's size. */
        s64 file_size = 0;
        R_TRY(Fs(obj).GetFileSize(std::addressof(file), std::addressof(file_size)));

        /* Send the header and file size. */
        R_TRY(db.AddDataHeader(m_request_header, file_size));

        WriteCallbackFile(CallbackType_ReadBegin, obj->GetName());
        ON_SCOPE_EXIT { WriteCallbackFile(CallbackType_ReadEnd, obj->GetName()); };

        auto mode = sphaira::thread::Mode::MultiThreaded;
        if (!Fs(obj).MultiThreadTransfer(file_size, true)) {
            mode = sphaira::thread::Mode::SingleThreadedIfSmaller;
        }

        R_TRY(sphaira::thread::Transfer(file_size,
            [this, &file, &obj](void* data, s64 off, s64 size, u64* bytes_read) -> Result {
                /* Get the next batch. */
                R_TRY(Fs(obj).ReadFile(std::addressof(file), off, data, size, FsReadOption_None, bytes_read));
                R_SUCCEED();
            },
            [this, &db](const void* data, s64 off, s64 size) -> Result {
                /* Write to output. */
                R_TRY(db.AddBuffer((const u8*)data, size));
                WriteCallbackProgress(CallbackType_ReadProgress, off, size);
                R_SUCCEED();
            }, mode
        ));

        /* Flush the data response. */
        R_TRY(db.Commit());

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::SendObjectInfo(PtpDataParser &rdp) {
        /* Prop list is reset on SendObjectInfo. */
        m_send_prop_list.reset();

        /* Get the storage ID and parent object and flush the request packet. */
        u32 storage_id, parent_object;
        R_TRY(rdp.Read(std::addressof(storage_id)));
        R_TRY(rdp.Read(std::addressof(parent_object)));
        R_TRY(rdp.Finalize());

        PtpDataParser dp(m_buffers->usb_bulk_read_buffer, std::addressof(m_usb_server));
        PtpObjectInfo info(DefaultObjectInfo);

        /* Ensure we have a data header. */
        PtpUsbBulkContainer data_header;
        R_TRY(dp.Read(std::addressof(data_header)));
        R_UNLESS(data_header.type == PtpUsbBulkContainerType_Data,  haze::ResultUnknownRequestType());
        R_UNLESS(data_header.code == m_request_header.code,         haze::ResultOperationNotSupported());
        R_UNLESS(data_header.trans_id == m_request_header.trans_id, haze::ResultOperationNotSupported());

        /* Read in the object info. */
        R_TRY(dp.Read(std::addressof(info.storage_id)));
        R_TRY(dp.Read(std::addressof(info.object_format)));
        R_TRY(dp.Read(std::addressof(info.protection_status)));
        R_TRY(dp.Read(std::addressof(info.object_compressed_size)));
        R_TRY(dp.Read(std::addressof(info.thumb_format)));
        R_TRY(dp.Read(std::addressof(info.thumb_compressed_size)));
        R_TRY(dp.Read(std::addressof(info.thumb_width)));
        R_TRY(dp.Read(std::addressof(info.thumb_height)));
        R_TRY(dp.Read(std::addressof(info.image_width)));
        R_TRY(dp.Read(std::addressof(info.image_height)));
        R_TRY(dp.Read(std::addressof(info.image_depth)));
        R_TRY(dp.Read(std::addressof(info.parent_object)));
        R_TRY(dp.Read(std::addressof(info.association_type)));
        R_TRY(dp.Read(std::addressof(info.association_desc)));
        R_TRY(dp.Read(std::addressof(info.sequence_number)));
        R_TRY(dp.ReadString(m_buffers->filename_string_buffer));
        R_TRY(dp.ReadString(m_buffers->capture_date_string_buffer));
        R_TRY(dp.ReadString(m_buffers->modification_date_string_buffer));
        R_TRY(dp.ReadString(m_buffers->keywords_string_buffer));
        R_TRY(dp.Finalize());

        /* Rewrite requests for creating in storage directories. */
        if (parent_object == PtpGetObjectHandles_RootParent) {
            parent_object = storage_id;
        }

        /* Check if we know about the parent object. If we don't, it's an error. */
        auto * const parentobj = m_object_database.GetObjectById(parent_object);
        R_UNLESS(parentobj != nullptr, haze::ResultInvalidObjectId());

        /* Make a new object with the intended name. */
        PtpNewObjectInfo new_object_info;
        new_object_info.storage_id       = parentobj->GetObjectId();
        new_object_info.parent_object_id = parent_object == storage_id ? 0 : parent_object;

        /* Create the object in the database. */
        PtpObject *obj;
        R_TRY(m_object_database.CreateOrFindObject(parentobj->GetName(), m_buffers->filename_string_buffer, parentobj->GetObjectId(), parentobj->GetStorageId(), std::addressof(obj)));

        /* Ensure we maintain a clean state on failure. */
        ON_RESULT_FAILURE { m_object_database.DeleteObject(obj); };

        /* Register the object with a new ID. */
        m_object_database.RegisterObject(obj);
        new_object_info.object_id = obj->GetObjectId();

        /* Create the object on the filesystem. */
        if (info.object_format == PtpObjectFormatCode_Association) {
            R_TRY(Fs(obj).CreateDirectory(obj->GetName()));
            WriteCallbackFile(CallbackType_CreateFolder, obj->GetName());
            m_send_object_id = 0;
        } else {
            R_TRY(Fs(obj).CreateFile(obj->GetName(), 0, 0));
            WriteCallbackFile(CallbackType_CreateFile, obj->GetName());
            m_send_object_id = new_object_info.object_id;
        }

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok, new_object_info));
    }

    Result PtpResponder::SendObject(PtpDataParser &rdp) {
        /* Reset SendObject object ID on exit. */
        ON_SCOPE_EXIT { m_send_object_id = 0; };

        R_TRY(rdp.Finalize());

        PtpDataParser dp(m_buffers->usb_bulk_read_buffer, std::addressof(m_usb_server));

        /* Ensure we have a data header. */
        PtpUsbBulkContainer data_header;
        R_TRY(dp.Read(std::addressof(data_header)));
        R_UNLESS(data_header.type == PtpUsbBulkContainerType_Data,  haze::ResultUnknownRequestType());
        R_UNLESS(data_header.code == m_request_header.code,         haze::ResultOperationNotSupported());
        R_UNLESS(data_header.trans_id == m_request_header.trans_id, haze::ResultOperationNotSupported());

        /* Check if we know about the object. If we don't, it's an error. */
        auto * const obj = m_object_database.GetObjectById(m_send_object_id);
        R_UNLESS(obj != nullptr, haze::ResultInvalidObjectId());

        /* Lock the object as a file. */
        FsFile file;
        R_TRY(Fs(obj).OpenFile(obj->GetName(), FsOpenMode_Write | FsOpenMode_Append, std::addressof(file)));

        /* Ensure we maintain a clean state on exit. */
        ON_SCOPE_EXIT { Fs(obj).CloseFile(std::addressof(file)); };

        /* Dummy file size for the threaded transfer. */
        auto file_size = 4_GB;
        u64 offset = 0;

        if (m_send_prop_list) {
            file_size = m_send_prop_list->size;
        } else {
            if (data_header.length > sizeof(PtpUsbBulkContainer)) {
                /* Got the real file size. */
                file_size = data_header.length - sizeof(PtpUsbBulkContainer);
                R_TRY(Fs(obj).SetFileSize(std::addressof(file), file_size));
            } else {
                /* Truncate the file after locking for write. */
                R_TRY(Fs(obj).SetFileSize(std::addressof(file), 0));
            }
        }

        /* Truncate the file to the received size. */
        ON_SCOPE_EXIT{
            if (offset != file_size) {
                Fs(obj).SetFileSize(std::addressof(file), offset);
            }
        };

        WriteCallbackFile(CallbackType_WriteBegin, obj->GetName());
        ON_SCOPE_EXIT { WriteCallbackFile(CallbackType_WriteEnd, obj->GetName()); };

        auto mode = sphaira::thread::Mode::MultiThreaded;
        if (!Fs(obj).MultiThreadTransfer(0, false)) {
            mode = sphaira::thread::Mode::SingleThreaded;
        }

        bool is_done = false;

        R_TRY(sphaira::thread::Transfer(file_size,
            [this, &dp, &is_done](void* data, s64 off, s64 size, u64* bytes_read) -> Result {
                if (is_done) {
                    *bytes_read = 0;
                    R_SUCCEED();
                }

                /* Read as many bytes as we can. */
                u32 bytes_received;
                const Result read_res = dp.ReadBuffer((u8*)data, size, std::addressof(bytes_received));
                *bytes_read = bytes_received;

                /* If we received fewer bytes than the batch size, we're done. */
                if (haze::ResultEndOfTransmission::Includes(read_res)) {
                    is_done = true;
                    R_SUCCEED();
                }

                R_RETURN(read_res);
            },
            [this, &file, &obj, &offset](const void* data, s64 off, s64 size) -> Result {
                /* Write to the file. */
                R_TRY(Fs(obj).WriteFile(std::addressof(file), off, data, size, 0));
                WriteCallbackProgress(CallbackType_WriteProgress, off, size);
                offset += size;
                R_SUCCEED();
            }, mode
        ));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::DeleteObject(PtpDataParser &dp) {
        /* Get the object ID and flush the request packet. */
        u32 object_id;
        R_TRY(dp.Read(std::addressof(object_id)));
        R_TRY(dp.Finalize());

        /* Disallow deleting the storage root. */
        const auto it = std::find_if(m_fs_entries.cbegin(), m_fs_entries.cend(), [object_id](auto& e){
            return object_id == e.storage_id;
        });
        R_UNLESS(it == m_fs_entries.cend(), haze::ResultInvalidObjectId());

        /* Check if we know about the object. If we don't, it's an error. */
        auto * const obj = m_object_database.GetObjectById(object_id);
        R_UNLESS(obj != nullptr, haze::ResultInvalidObjectId());

        /* Figure out what type of object this is. */
        FsDirEntryType entry_type;
        R_TRY(Fs(obj).GetEntryType(obj->GetName(), std::addressof(entry_type)));

        /* Remove the object from the filesystem. */
        if (entry_type == FsDirEntryType_Dir) {
            WriteCallbackFile(CallbackType_DeleteFolder, obj->GetName());
            R_TRY(Fs(obj).DeleteDirectoryRecursively(obj->GetName()));
        } else {
            WriteCallbackFile(CallbackType_DeleteFile, obj->GetName());
            R_TRY(Fs(obj).DeleteFile(obj->GetName()));
        }

        /* Remove the object from the database. */
        m_object_database.DeleteObject(obj);

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

}
