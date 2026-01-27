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

namespace haze {

    Result PtpResponder::GetObjectPropsSupported(PtpDataParser &dp) {
        R_TRY(dp.Finalize());

        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        /* Write information about all object properties we can support. */
        R_TRY(db.WriteVariableLengthData(m_request_header, [&] {
            R_RETURN(db.AddArray(SupportedObjectProperties, util::size(SupportedObjectProperties)));
        }));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::GetObjectPropDesc(PtpDataParser &dp) {
        PtpObjectPropertyCode property_code;
        u16 object_format;

        R_TRY(dp.Read(std::addressof(property_code)));
        R_TRY(dp.Read(std::addressof(object_format)));
        R_TRY(dp.Finalize());

        /* Ensure we have a valid property code before continuing. */
        R_UNLESS(IsSupportedObjectPropertyCode(property_code), haze::ResultUnknownPropertyCode());

        /* Begin writing information about the property code. */
        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        R_TRY(db.WriteVariableLengthData(m_request_header, [&] {
            R_TRY(db.Add(property_code));

            /* Each property code corresponds to a different pattern, which contains the data type, */
            /* whether the property can be set for an object, and the default value of the property. */
            switch (property_code) {
                case PtpObjectPropertyCode_PersistentUniqueObjectIdentifier:
                    {
                        R_TRY(db.Add(PtpDataTypeCode_U128));
                        R_TRY(db.Add(PtpPropertyGetSetFlag_Get));
                        R_TRY(db.Add<u128>(0));
                    }
                case PtpObjectPropertyCode_ObjectSize:
                    {
                        R_TRY(db.Add(PtpDataTypeCode_U64));
                        R_TRY(db.Add(PtpPropertyGetSetFlag_Get));
                        R_TRY(db.Add<u64>(0));
                    }
                    break;
                case PtpObjectPropertyCode_StorageId:
                    {
                        R_TRY(db.Add(PtpDataTypeCode_U32));
                        R_TRY(db.Add(PtpPropertyGetSetFlag_Get));
                        // TODO(TJ): what should this value here be?
                        R_TRY(db.Add(StorageId_DefaultStorage));
                    }
                    break;
                case PtpObjectPropertyCode_ParentObject:
                    {
                        R_TRY(db.Add(PtpDataTypeCode_U32));
                        R_TRY(db.Add(PtpPropertyGetSetFlag_Get));
                        // TODO(TJ): what should this value here be?
                        R_TRY(db.Add(StorageId_DefaultStorage));
                    }
                    break;
                case PtpObjectPropertyCode_ObjectFormat:
                    {
                        R_TRY(db.Add(PtpDataTypeCode_U16));
                        R_TRY(db.Add(PtpPropertyGetSetFlag_Get));
                        R_TRY(db.Add(PtpObjectFormatCode_Undefined));
                    }
                    break;
                case PtpObjectPropertyCode_ObjectFileName:
                    {
                        R_TRY(db.Add(PtpDataTypeCode_String));
                        R_TRY(db.Add(PtpPropertyGetSetFlag_GetSet));
                        R_TRY(db.AddString(""));
                    }
                    break;
                HAZE_UNREACHABLE_DEFAULT_CASE();
            }

            /* Group code is a required part of the response, but doesn't seem to be used for anything. */
            R_TRY(db.Add(PtpPropertyGroupCode_Default));

            /* We don't use the form flag. */
            R_TRY(db.Add(PtpPropertyFormFlag_None));

            R_SUCCEED();
        }));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::GetObjectPropValue(PtpDataParser &dp) {
        u32 object_id;
        PtpObjectPropertyCode property_code;

        R_TRY(dp.Read(std::addressof(object_id)));
        R_TRY(dp.Read(std::addressof(property_code)));
        R_TRY(dp.Finalize());

        /* Ensure we have a valid property code before continuing. */
        R_UNLESS(IsSupportedObjectPropertyCode(property_code), haze::ResultUnknownPropertyCode());

        /* Check if we know about the object. If we don't, it's an error. */
        auto * const obj = m_object_database.GetObjectById(object_id);
        R_UNLESS(obj != nullptr, haze::ResultInvalidObjectId());

        /* Define helper for getting the object type. */
        const auto GetObjectType = [&] (FsDirEntryType *out_entry_type) {
            R_RETURN(Fs(obj).GetEntryType(obj->GetName(), out_entry_type));
        };

        /* Define helper for getting the object size. */
        const auto GetObjectSize = [&] (s64 *out_size) {
            *out_size = 0;

            /* Check if this is a directory. */
            FsDirEntryType entry_type;
            R_TRY(GetObjectType(std::addressof(entry_type)));

            /* If it is, we're done. */
            R_SUCCEED_IF(entry_type == FsDirEntryType_Dir);

            /* Otherwise, open as a file. */
            FsFile file;
            R_TRY(Fs(obj).OpenFile(obj->GetName(), FsOpenMode_Read, std::addressof(file)));

            /* Ensure we maintain a clean state on exit. */
            ON_SCOPE_EXIT { Fs(obj).CloseFile(std::addressof(file)); };

            R_RETURN(Fs(obj).GetFileSize(std::addressof(file), out_size));
        };

        /* Begin writing the requested object property. */
        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        R_TRY(db.WriteVariableLengthData(m_request_header, [&] {
            switch (property_code) {
                case PtpObjectPropertyCode_PersistentUniqueObjectIdentifier:
                    {
                        R_TRY(db.Add<u128>(object_id));
                    }
                    break;
                case PtpObjectPropertyCode_ObjectSize:
                    {
                        s64 size;
                        R_TRY(GetObjectSize(std::addressof(size)));
                        R_TRY(db.Add<u64>(size));
                    }
                    break;
                case PtpObjectPropertyCode_StorageId:
                    {
                        R_TRY(db.Add(obj->GetStorageId()));
                    }
                    break;
                case PtpObjectPropertyCode_ParentObject:
                    {
                        R_TRY(db.Add(obj->GetParentId()));
                    }
                    break;
                case PtpObjectPropertyCode_ObjectFormat:
                    {
                        FsDirEntryType entry_type;
                        R_TRY(GetObjectType(std::addressof(entry_type)));
                        R_TRY(db.Add(entry_type == FsDirEntryType_File ? PtpObjectFormatCode_Undefined : PtpObjectFormatCode_Association));
                    }
                    break;
                case PtpObjectPropertyCode_ObjectFileName:
                    {
                        R_TRY(db.AddString(std::strrchr(obj->GetName(), '/') + 1));
                    }
                    break;
                HAZE_UNREACHABLE_DEFAULT_CASE();
            }

            R_SUCCEED();
        }));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::GetObjectPropList(PtpDataParser &dp) {
        u32 object_id;
        u32 object_format;
        s32 property_code;
        s32 group_code;
        s32 depth;

        R_TRY(dp.Read(std::addressof(object_id)));
        R_TRY(dp.Read(std::addressof(object_format)));
        R_TRY(dp.Read(std::addressof(property_code)));
        R_TRY(dp.Read(std::addressof(group_code)));
        R_TRY(dp.Read(std::addressof(depth)));
        R_TRY(dp.Finalize());

        /* Ensure format is unspecified. */
        R_UNLESS(object_format == 0, haze::ResultInvalidArgument());

        /* Ensure we have a valid property code. */
        R_UNLESS(property_code == -1 || IsSupportedObjectPropertyCode(PtpObjectPropertyCode(property_code)), haze::ResultUnknownPropertyCode());

        /* Ensure group code is the default. */
        R_UNLESS(group_code == PtpPropertyGroupCode_Default, haze::ResultGroupSpecified());

        /* Ensure depth is 0. */
        R_UNLESS(depth == 0, haze::ResultDepthSpecified());

        /* Check if we know about the object. If we don't, it's an error. */
        auto * const obj = m_object_database.GetObjectById(object_id);
        R_UNLESS(obj != nullptr, haze::ResultInvalidObjectId());

        /* Define helper for getting the object type. */
        const auto GetObjectType = [&] (FsDirEntryType *out_entry_type) {
            R_RETURN(Fs(obj).GetEntryType(obj->GetName(), out_entry_type));
        };

        /* Define helper for getting the object size. */
        const auto GetObjectSize = [&] (s64 *out_size) {
            *out_size = 0;

            /* Check if this is a directory. */
            FsDirEntryType entry_type;
            R_TRY(GetObjectType(std::addressof(entry_type)));

            /* If it is, we're done. */
            R_SUCCEED_IF(entry_type == FsDirEntryType_Dir);

            /* Otherwise, open as a file. */
            FsFile file;
            R_TRY(Fs(obj).OpenFile(obj->GetName(), FsOpenMode_Read, std::addressof(file)));

            /* Ensure we maintain a clean state on exit. */
            ON_SCOPE_EXIT { Fs(obj).CloseFile(std::addressof(file)); };

            R_RETURN(Fs(obj).GetFileSize(std::addressof(file), out_size));
        };

        /* Define helper for determining if the property should be included. */
        const auto ShouldIncludeProperty = [&] (PtpObjectPropertyCode code) {
            /* If all properties were requested, or it was the requested property, we should include the property. */
            return property_code == -1 || code == property_code;
        };

        /* Determine how many output elements we will report. */
        u32 num_output_elements = 0;
        for (const auto obj_property : SupportedObjectProperties) {
            if (ShouldIncludeProperty(obj_property)) {
                num_output_elements++;
            }
        }

        /* Begin writing the requested object properties. */
        PtpDataBuilder db(m_buffers->usb_bulk_write_buffer, std::addressof(m_usb_server));

        R_TRY(db.WriteVariableLengthData(m_request_header, [&] {
            /* Report the number of elements. */
            R_TRY(db.Add(num_output_elements));

            for (const auto obj_property : SupportedObjectProperties) {
                if (!ShouldIncludeProperty(obj_property)) {
                    continue;
                }

                /* Write the object handle. */
                R_TRY(db.Add<u32>(object_id));

                /* Write the property code. */
                R_TRY(db.Add<u16>(obj_property));

                /* Write the property value. */
                switch (obj_property) {
                    case PtpObjectPropertyCode_PersistentUniqueObjectIdentifier:
                        {
                            R_TRY(db.Add(PtpDataTypeCode_U128));
                            R_TRY(db.Add<u128>(object_id));
                        }
                        break;
                    case PtpObjectPropertyCode_ObjectSize:
                        {
                            s64 size;
                            R_TRY(GetObjectSize(std::addressof(size)));
                            R_TRY(db.Add(PtpDataTypeCode_U64));
                            R_TRY(db.Add<u64>(size));
                        }
                        break;
                    case PtpObjectPropertyCode_StorageId:
                        {
                            R_TRY(db.Add(PtpDataTypeCode_U32));
                            R_TRY(db.Add(obj->GetStorageId()));
                        }
                        break;
                    case PtpObjectPropertyCode_ParentObject:
                        {
                            R_TRY(db.Add(PtpDataTypeCode_U32));
                            R_TRY(db.Add(obj->GetParentId()));
                        }
                        break;
                    case PtpObjectPropertyCode_ObjectFormat:
                        {
                            FsDirEntryType entry_type;
                            R_TRY(GetObjectType(std::addressof(entry_type)));
                            R_TRY(db.Add(PtpDataTypeCode_U16));
                            R_TRY(db.Add(entry_type == FsDirEntryType_File ? PtpObjectFormatCode_Undefined : PtpObjectFormatCode_Association));
                        }
                        break;
                    case PtpObjectPropertyCode_ObjectFileName:
                        {
                            R_TRY(db.Add(PtpDataTypeCode_String));
                            R_TRY(db.AddString(std::strrchr(obj->GetName(), '/') + 1));
                        }
                        break;
                    HAZE_UNREACHABLE_DEFAULT_CASE();
                }
            }

            R_SUCCEED();
        }));

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

    Result PtpResponder::SendObjectPropList(PtpDataParser &rdp) {
        /* Prop list is reset on SendObjectPropList. */
        m_send_prop_list.reset();

        u32 storage_id;
        u32 parent_object;
        u32 format_code;
        u32 object_size_msb;
        u32 object_size_lsb;

        R_TRY(rdp.Read(std::addressof(storage_id)));
        R_TRY(rdp.Read(std::addressof(parent_object)));
        R_TRY(rdp.Read(std::addressof(format_code)));
        R_TRY(rdp.Read(std::addressof(object_size_msb)));
        R_TRY(rdp.Read(std::addressof(object_size_lsb)));
        R_TRY(rdp.Finalize());

        /* Rewrite requests for creating in storage directories. */
        if (parent_object == PtpGetObjectHandles_RootParent) {
            parent_object = storage_id;
        }

        /* Check if we know about the parent object. If we don't, it's an error. */
        auto * const parentobj = m_object_database.GetObjectById(parent_object);
        R_UNLESS(parentobj != nullptr, haze::ResultInvalidObjectId());

        PtpDataParser dp(m_buffers->usb_bulk_read_buffer, std::addressof(m_usb_server));

        /* Ensure we have a data header. */
        PtpUsbBulkContainer data_header;
        R_TRY(dp.Read(std::addressof(data_header)));
        R_UNLESS(data_header.type == PtpUsbBulkContainerType_Data,  haze::ResultUnknownRequestType());
        R_UNLESS(data_header.code == m_request_header.code,         haze::ResultOperationNotSupported());
        R_UNLESS(data_header.trans_id == m_request_header.trans_id, haze::ResultOperationNotSupported());

        /* Get the number of properties */
        u32 num_properties;
        R_TRY(dp.Read(std::addressof(num_properties)));

        for (u32 i = 0; i < num_properties; i++) {
            /* Read the object handle. */
            u32 object_id;
            R_TRY(dp.Read(std::addressof(object_id)));

            /* Read the property code. */
            u16 obj_property;
            R_TRY(dp.Read(std::addressof(obj_property)));

            /* Read the type. */
            PtpDataTypeCode type;
            R_TRY(dp.Read(std::addressof(type)));

            switch (obj_property) {
                case PtpObjectPropertyCode_ObjectFileName:
                    {
                        R_UNLESS(type == PtpDataTypeCode_String, haze::ResultUnknownPropertyCode());
                        R_TRY((dp.ReadString(m_buffers->filename_string_buffer)));
                    }
                    break;
                default:
                    R_THROW(haze::ResultUnknownPropertyCode());
            }
        }
        R_TRY(dp.Finalize());

        /* Ensure we can actually process the new name. */
        const bool is_empty         = m_buffers->filename_string_buffer[0] == '\x00';
        const bool contains_slashes = std::strchr(m_buffers->filename_string_buffer, '/') != nullptr;
        R_UNLESS(!is_empty && !contains_slashes, haze::ResultInvalidPropertyValue());

        /* Add a new object in the database with the new name. */
        PtpObject *newobj;
        R_TRY(m_object_database.CreateOrFindObject(parentobj->GetName(), m_buffers->filename_string_buffer, parentobj->GetObjectId(), parentobj->GetStorageId(), std::addressof(newobj)));

        /* Create prop list. */
        ObjectPropList prop_list{};
        prop_list.size = ((u64)object_size_msb << 32) | object_size_lsb;
        m_send_prop_list = prop_list;

        /* Make a new object with the intended name. */
        PtpNewObjectInfo new_object_info;
        new_object_info.storage_id       = parentobj->GetObjectId();
        new_object_info.parent_object_id = parent_object == storage_id ? 0 : parent_object;

        /* Ensure we maintain a clean state on failure. */
        ON_RESULT_FAILURE { m_object_database.DeleteObject(newobj); };

        /* Register the object with a new ID. */
        m_object_database.RegisterObject(newobj);
        new_object_info.object_id = newobj->GetObjectId();

        /* Create the object on the filesystem. */
        if (format_code == PtpObjectFormatCode_Association) {
            R_TRY(Fs(newobj).CreateDirectory(newobj->GetName()));
            WriteCallbackFile(CallbackType_CreateFolder, newobj->GetName());
            m_send_object_id = 0;
        } else {
            u32 flags = 0;
            if (prop_list.size >= 4_GB) {
                flags = FsCreateOption_BigFile;
            }

            R_TRY(Fs(newobj).CreateFile(newobj->GetName(), prop_list.size, flags));
            WriteCallbackFile(CallbackType_CreateFile, newobj->GetName());
            m_send_object_id = new_object_info.object_id;
        }

        /* Save prop list and return success. */
        m_send_prop_list = prop_list;
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok, new_object_info));
    }

    Result PtpResponder::SetObjectPropValue(PtpDataParser &rdp) {
        u32 object_id;
        PtpObjectPropertyCode property_code;

        R_TRY(rdp.Read(std::addressof(object_id)));
        R_TRY(rdp.Read(std::addressof(property_code)));
        R_TRY(rdp.Finalize());

        PtpDataParser dp(m_buffers->usb_bulk_read_buffer, std::addressof(m_usb_server));

        /* Ensure we have a data header. */
        PtpUsbBulkContainer data_header;
        R_TRY(dp.Read(std::addressof(data_header)));
        R_UNLESS(data_header.type == PtpUsbBulkContainerType_Data,      haze::ResultUnknownRequestType());
        R_UNLESS(data_header.code == m_request_header.code,             haze::ResultOperationNotSupported());
        R_UNLESS(data_header.trans_id == m_request_header.trans_id,     haze::ResultOperationNotSupported());

        /* Ensure we have a valid property code before continuing. */
        R_UNLESS(property_code == PtpObjectPropertyCode_ObjectFileName, haze::ResultUnknownPropertyCode());

        /* Check if we know about the object. If we don't, it's an error. */
        auto * const obj = m_object_database.GetObjectById(object_id);
        R_UNLESS(obj != nullptr, haze::ResultInvalidObjectId());

        /* We are reading a file name. */
        R_TRY(dp.ReadString(m_buffers->filename_string_buffer));
        R_TRY(dp.Finalize());

        /* Ensure we can actually process the new name. */
        const bool is_empty         = m_buffers->filename_string_buffer[0] == '\x00';
        const bool contains_slashes = std::strchr(m_buffers->filename_string_buffer, '/') != nullptr;
        R_UNLESS(!is_empty && !contains_slashes, haze::ResultInvalidPropertyValue());

        /* Add a new object in the database with the new name. */
        PtpObject *newobj;
        {
            /* Find the last path separator in the existing object name. */
            char *pathsep = std::strrchr(obj->m_name, '/');
            HAZE_ASSERT(pathsep != nullptr);

            /* Temporarily mark the path separator as null to facilitate processing. */
            *pathsep = '\x00';
            ON_SCOPE_EXIT { *pathsep = '/'; };

            R_TRY(m_object_database.CreateOrFindObject(obj->GetName(), m_buffers->filename_string_buffer, obj->GetParentId(), obj->GetStorageId(), std::addressof(newobj)));
        }

        {
            /* Ensure we maintain a clean state on failure. */
            ON_RESULT_FAILURE {
                if (!newobj->GetIsRegistered()) {
                    /* Only delete if the object was not registered. */
                    /* Otherwise, we would remove an object that still exists. */
                    m_object_database.DeleteObject(newobj);
                }
            };

            /* Get the old object type. */
            FsDirEntryType entry_type;
            R_TRY(Fs(obj).GetEntryType(obj->GetName(), std::addressof(entry_type)));

            /* Attempt to rename the object on the filesystem. */
            if (entry_type == FsDirEntryType_Dir) {
                R_TRY(Fs(obj).RenameDirectory(obj->GetName(), newobj->GetName()));
                WriteCallbackRename(CallbackType_RenameFolder, obj->GetName(), newobj->GetName());
            } else {
                R_TRY(Fs(obj).RenameFile(obj->GetName(), newobj->GetName()));
                WriteCallbackRename(CallbackType_RenameFile, obj->GetName(), newobj->GetName());
            }
        }

        /* Unregister and free the old object. */
        m_object_database.DeleteObject(obj);

        /* Register the new object. */
        m_object_database.RegisterObject(newobj, object_id);

        /* Write the success response. */
        R_RETURN(this->WriteResponse(PtpResponseCode_Ok));
    }

}
